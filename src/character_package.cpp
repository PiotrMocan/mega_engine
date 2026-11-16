#include "character_package.hpp"

#include "character_strand_groom.hpp"
#include "character_wardrobe.hpp"
#include "cooked_audio_asset.hpp"
#include "cooked_texture_asset.hpp"
#include "game_ui_runtime.hpp"
#include "localization.hpp"
#include "png_encoder.hpp"
#include "prefab_io.hpp"
#include "runtime_assets.hpp"
#include "scene_components.hpp"
#include "scene_document.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <mutex>
#include <sstream>
#include <system_error>
#include <unordered_map>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace vcr {
namespace {

using Json = nlohmann::json;

constexpr std::uint32_t max_sculpt_strokes = 2048U;
constexpr std::uint32_t max_paint_strokes = 4096U;
constexpr std::uint32_t max_dialogue_nodes = 64U;
constexpr std::uint32_t max_dialogue_choices_per_node = 4U;

using TemplateRegion = WardrobeTemplateRegion;

constexpr std::uint32_t template_morph_count = 18U;
constexpr std::uint32_t template_part_count = 5U;
constexpr std::uint32_t template_mask_size = 512U;
constexpr std::uint32_t template_landmark_chain_count = 6U;

[[nodiscard]] bool checked_mul_size(
    const std::size_t a,
    const std::size_t b,
    std::size_t& out) noexcept {
    if (a != 0U && b > std::numeric_limits<std::size_t>::max() / a) {
        return false;
    }
    out = a * b;
    return true;
}

[[nodiscard]] float clamp_finite(
    const float v,
    const float lo,
    const float hi,
    const float fallback) noexcept {
    if (!std::isfinite(v)) {
        return fallback;
    }
    return std::clamp(v, lo, hi);
}

[[nodiscard]] glm::vec3 clamp_vec3_color(
    glm::vec3 c,
    const glm::vec3 fallback) noexcept {
    c.x = clamp_finite(c.x, 0.0F, 1.0F, fallback.x);
    c.y = clamp_finite(c.y, 0.0F, 1.0F, fallback.y);
    c.z = clamp_finite(c.z, 0.0F, 1.0F, fallback.z);
    return c;
}

[[nodiscard]] glm::vec4 clamp_vec4_color(
    glm::vec4 c,
    const glm::vec4 fallback) noexcept {
    c.x = clamp_finite(c.x, 0.0F, 1.0F, fallback.x);
    c.y = clamp_finite(c.y, 0.0F, 1.0F, fallback.y);
    c.z = clamp_finite(c.z, 0.0F, 1.0F, fallback.z);
    c.w = clamp_finite(c.w, 0.0F, 1.0F, fallback.w);
    return c;
}

[[nodiscard]] float srgb_to_linear_channel(const float c) noexcept {
    const float clamped = std::clamp(c, 0.0F, 1.0F);
    return clamped <= 0.04045F
        ? clamped / 12.92F
        : std::pow((clamped + 0.055F) / 1.055F, 2.4F);
}

[[nodiscard]] float linear_to_srgb_channel(const float c) noexcept {
    const float clamped = std::clamp(c, 0.0F, 1.0F);
    return clamped <= 0.0031308F
        ? clamped * 12.92F
        : 1.055F * std::pow(clamped, 1.0F / 2.4F) - 0.055F;
}

[[nodiscard]] std::uint8_t float_to_unorm8(const float v) noexcept {
    const float clamped = std::clamp(v, 0.0F, 1.0F);
    return static_cast<std::uint8_t>(std::lround(clamped * 255.0F));
}

[[nodiscard]] std::string utf8_from_path(const std::filesystem::path& path) {
    const std::u8string u8 = path.u8string();
    std::string out(u8.size(), '\0');
    std::transform(u8.begin(), u8.end(), out.begin(), [](const char8_t ch) {
        return static_cast<char>(ch);
    });
    return out;
}

[[nodiscard]] Json parse_extra_object_or_empty(const std::string_view raw) {
    if (raw.empty()) {
        return Json::object();
    }
    try {
        Json parsed = Json::parse(raw);
        if (parsed.is_object()) {
            return parsed;
        }
    } catch (const Json::exception&) {
    }
    return Json::object();
}

void merge_extra_object(Json& target, const std::string_view extra_raw) {
    Json extra = parse_extra_object_or_empty(extra_raw);
    for (auto it = extra.begin(); it != extra.end(); ++it) {
        if (!target.contains(it.key())) {
            target[it.key()] = it.value();
        }
    }
}

[[nodiscard]] std::string extract_unknown_keys_json(
    const Json& obj,
    std::initializer_list<const char*> known_keys) {
    if (!obj.is_object()) {
        return "{}";
    }
    Json extra = Json::object();
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        bool known = false;
        for (const char* k : known_keys) {
            if (it.key() == k) {
                known = true;
                break;
            }
        }
        if (!known) {
            extra[it.key()] = it.value();
        }
    }
    return extra.dump();
}

[[nodiscard]] Json vec2_to_json(const glm::vec2& v) {
    return Json::array({v.x, v.y});
}

[[nodiscard]] Json vec3_to_json(const glm::vec3& v) {
    return Json::array({v.x, v.y, v.z});
}

[[nodiscard]] Json vec4_to_json(const glm::vec4& v) {
    return Json::array({v.x, v.y, v.z, v.w});
}

[[nodiscard]] bool read_vec2_json(const Json& node, glm::vec2& out) {
    if (!node.is_array() || node.size() != 2U ||
        !node[0].is_number() || !node[1].is_number()) {
        return false;
    }
    out = glm::vec2(node[0].get<float>(), node[1].get<float>());
    return true;
}

[[nodiscard]] bool read_vec3_json(const Json& node, glm::vec3& out) {
    if (!node.is_array() || node.size() != 3U ||
        !node[0].is_number() || !node[1].is_number() || !node[2].is_number()) {
        return false;
    }
    out = glm::vec3(
        node[0].get<float>(), node[1].get<float>(), node[2].get<float>());
    return true;
}

[[nodiscard]] bool read_vec4_json(const Json& node, glm::vec4& out) {
    if (!node.is_array() || node.size() != 4U ||
        !node[0].is_number() || !node[1].is_number() ||
        !node[2].is_number() || !node[3].is_number()) {
        return false;
    }
    out = glm::vec4(
        node[0].get<float>(),
        node[1].get<float>(),
        node[2].get<float>(),
        node[3].get<float>());
    return true;
}

void write_u32_le(std::ostream& os, const std::uint32_t v) {
    const std::array<char, 4U> bytes{
        static_cast<char>(v & 0xFFU),
        static_cast<char>((v >> 8U) & 0xFFU),
        static_cast<char>((v >> 16U) & 0xFFU),
        static_cast<char>((v >> 24U) & 0xFFU),
    };
    os.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void write_u16_le(std::ostream& os, const std::uint16_t v) {
    const std::array<char, 2U> bytes{
        static_cast<char>(v & 0xFFU),
        static_cast<char>((v >> 8U) & 0xFFU),
    };
    os.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void write_i16_le(std::ostream& os, const std::int16_t v) {
    write_u16_le(os, static_cast<std::uint16_t>(v));
}

void write_u64_le(std::ostream& os, const std::uint64_t v) {
    std::array<char, 8U> bytes{};
    for (std::size_t i = 0U; i < 8U; ++i) {
        bytes[i] = static_cast<char>((v >> (i * 8U)) & 0xFFU);
    }
    os.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void write_f32_le(std::ostream& os, const float value) {
    const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
    write_u32_le(os, bits);
}

[[nodiscard]] bool write_text_file_atomic(
    const std::filesystem::path& path,
    const std::string_view contents,
    std::string& error) {
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) {
            error = "Failed to create directory '" +
                    utf8_from_path(path.parent_path()) + "': " + ec.message();
            return false;
        }
    }
    const std::filesystem::path temp_path =
        path.string() + ".tmp_vcrchar";
    {
        std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "Failed to open temporary file '" +
                    utf8_from_path(temp_path) + "' for writing.";
            return false;
        }
        out.write(
            contents.data(),
            static_cast<std::streamsize>(contents.size()));
        out.flush();
        if (!out) {
            error = "Failed while writing temporary file '" +
                    utf8_from_path(temp_path) + "'.";
            out.close();
            std::filesystem::remove(temp_path, ec);
            return false;
        }
    }
    std::filesystem::rename(temp_path, path, ec);
    if (ec) {
#if defined(_WIN32)
        if (MoveFileExW(
                temp_path.c_str(),
                path.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0) {
            return true;
        }
#endif
        error = "Failed to commit file '" + utf8_from_path(path) + "': " +
                ec.message();
        std::filesystem::remove(temp_path, ec);
        return false;
    }
    return true;
}

[[nodiscard]] bool write_bytes_file_atomic(
    const std::filesystem::path& path,
    const std::span<const std::uint8_t> bytes,
    std::string& error) {
    return write_text_file_atomic(
        path,
        std::string_view(
            reinterpret_cast<const char*>(bytes.data()), bytes.size()),
        error);
}

[[nodiscard]] std::uint32_t hash_u32(std::uint32_t x) noexcept {
    x ^= x >> 16U;
    x *= 0x7FEB352DU;
    x ^= x >> 15U;
    x *= 0x846CA68BU;
    x ^= x >> 16U;
    return x;
}

[[nodiscard]] float hash_float01(
    const std::uint32_t x,
    const std::uint32_t y,
    const std::uint32_t seed) noexcept {
    const std::uint32_t h = hash_u32(
        x * 374761393U + y * 668265263U + seed * 1442695041U);
    return static_cast<float>(h & 0x00FFFFFFU) / 16777215.0F;
}

[[nodiscard]] float value_noise_2d(
    const float u,
    const float v,
    const std::uint32_t seed) noexcept {
    const float fx = std::floor(u);
    const float fy = std::floor(v);
    const auto ix = static_cast<std::int32_t>(fx);
    const auto iy = static_cast<std::int32_t>(fy);
    const float tx = u - fx;
    const float ty = v - fy;
    const float sx = tx * tx * (3.0F - 2.0F * tx);
    const float sy = ty * ty * (3.0F - 2.0F * ty);
    const float c00 = hash_float01(static_cast<std::uint32_t>(ix), static_cast<std::uint32_t>(iy), seed);
    const float c10 = hash_float01(static_cast<std::uint32_t>(ix + 1), static_cast<std::uint32_t>(iy), seed);
    const float c01 = hash_float01(static_cast<std::uint32_t>(ix), static_cast<std::uint32_t>(iy + 1), seed);
    const float c11 = hash_float01(static_cast<std::uint32_t>(ix + 1), static_cast<std::uint32_t>(iy + 1), seed);
    const float a = c00 + (c10 - c00) * sx;
    const float b = c01 + (c11 - c01) * sx;
    return a + (b - a) * sy;
}

// ============================================================================
// Skeleton Construction (38 Body Bones + 12 Facial Mini-Rig Bones = 50 Bones)
// ============================================================================

struct BoneModelSpec {
    const char* name;
    std::int32_t parent;
    glm::vec3 model_pos;
};

[[nodiscard]] std::array<BoneModelSpec, character_total_bone_count>
fallback_character_bone_layout(const CharacterBodyParams& body) noexcept {
    const float h = body.height_meters / 1.78F;
    const float sw = 0.85F + body.shoulder_width * 0.45F;
    const float arm_scale = h * (0.88F + body.arm_length * 0.24F);
    const float leg_scale = h * (0.88F + body.leg_length * 0.24F);
    const float head_scale = body.face.head_scale;
    const float jaw_drop = 0.028F * head_scale * h;

    const float pelvis_y = 0.92F * leg_scale;
    const float spine1_y = pelvis_y + 0.12F * h;
    const float spine2_y = pelvis_y + 0.27F * h;
    const float spine3_y = pelvis_y + 0.43F * h;
    const float neck_y = pelvis_y + 0.54F * h;
    const float head_y = pelvis_y + 0.65F * h;
    const float hip_x = 0.095F + (body.hip_width - 0.5F) * 0.04F;
    const float clav_x = 0.055F * sw;
    const float shld_x = 0.185F * sw;
    const float elbow_x = shld_x + 0.26F * arm_scale;
    const float wrist_x = elbow_x + 0.24F * arm_scale;

    std::array<BoneModelSpec, character_total_bone_count> bones{{
        {"Root", -1, {0.0F, 0.0F, 0.0F}},                                  // 0
        {"Pelvis", 0, {0.0F, pelvis_y, 0.0F}},                             // 1
        {"Spine_01", 1, {0.0F, spine1_y, 0.0F}},                           // 2
        {"Spine_02", 2, {0.0F, spine2_y, 0.0F}},                           // 3
        {"Spine_03", 3, {0.0F, spine3_y, 0.0F}},                           // 4
        {"Neck", 4, {0.0F, neck_y, 0.0F}},                                 // 5
        {"Head", 5, {0.0F, head_y, 0.0F}},                                 // 6
        {"Clavicle_L", 4, {clav_x, spine3_y + 0.04F * h, 0.0F}},           // 7
        {"UpperArm_L", 7, {shld_x, spine3_y + 0.02F * h, 0.0F}},           // 8
        {"Forearm_L", 8, {elbow_x, spine3_y - 0.01F * h, 0.01F}},          // 9
        {"Hand_L", 9, {wrist_x, spine3_y - 0.02F * h, 0.015F}},            // 10
        {"Thumb_L", 10, {wrist_x + 0.04F * arm_scale, spine3_y - 0.03F * h, 0.03F}}, // 11
        {"Index_L", 10, {wrist_x + 0.07F * arm_scale, spine3_y - 0.02F * h, 0.02F}}, // 12
        {"Middle_L", 10, {wrist_x + 0.08F * arm_scale, spine3_y - 0.02F * h, 0.005F}}, // 13
        {"Pinky_L", 10, {wrist_x + 0.07F * arm_scale, spine3_y - 0.025F * h, -0.02F}}, // 14
        {"Clavicle_R", 4, {-clav_x, spine3_y + 0.04F * h, 0.0F}},          // 15
        {"UpperArm_R", 15, {-shld_x, spine3_y + 0.02F * h, 0.0F}},         // 16
        {"Forearm_R", 16, {-elbow_x, spine3_y - 0.01F * h, 0.01F}},        // 17
        {"Hand_R", 17, {-wrist_x, spine3_y - 0.02F * h, 0.015F}},          // 18
        {"Thumb_R", 18, {-wrist_x - 0.04F * arm_scale, spine3_y - 0.03F * h, 0.03F}}, // 19
        {"Index_R", 18, {-wrist_x - 0.07F * arm_scale, spine3_y - 0.02F * h, 0.02F}}, // 20
        {"Middle_R", 18, {-wrist_x - 0.08F * arm_scale, spine3_y - 0.02F * h, 0.005F}}, // 21
        {"Pinky_R", 18, {-wrist_x - 0.07F * arm_scale, spine3_y - 0.025F * h, -0.02F}}, // 22
        {"Thigh_L", 1, {hip_x, pelvis_y - 0.04F * leg_scale, 0.0F}},       // 23
        {"Shin_L", 23, {hip_x, pelvis_y * 0.50F, 0.015F}},                 // 24
        {"Foot_L", 24, {hip_x, 0.065F * h, 0.0F}},                         // 25
        {"Toe_L", 25, {hip_x, 0.02F * h, 0.12F * h}},                      // 26
        {"Thigh_R", 1, {-hip_x, pelvis_y - 0.04F * leg_scale, 0.0F}},      // 27
        {"Shin_R", 27, {-hip_x, pelvis_y * 0.50F, 0.015F}},                // 28
        {"Foot_R", 28, {-hip_x, 0.065F * h, 0.0F}},                        // 29
        {"Toe_R", 29, {-hip_x, 0.02F * h, 0.12F * h}},                     // 30
        {"ShoulderTwist_L", 8, {shld_x + 0.10F * arm_scale, spine3_y + 0.01F * h, 0.0F}}, // 31
        {"ForearmTwist_L", 9, {elbow_x + 0.10F * arm_scale, spine3_y - 0.015F * h, 0.01F}}, // 32
        {"ShoulderTwist_R", 16, {-shld_x - 0.10F * arm_scale, spine3_y + 0.01F * h, 0.0F}}, // 33
        {"ForearmTwist_R", 17, {-elbow_x - 0.10F * arm_scale, spine3_y - 0.015F * h, 0.01F}}, // 34
        {"Pectoral_L", 4, {0.07F * sw, spine3_y - 0.05F * h, 0.09F}},      // 35
        {"Pectoral_R", 4, {-0.07F * sw, spine3_y - 0.05F * h, 0.09F}},     // 36
        {"TailCoatBase", 1, {0.0F, pelvis_y - 0.05F * h, -0.08F}},         // 37
        // --- 12-bone classic facial mini-rig (bones 38..49) ---
        {"Jaw", 6, {0.0F, head_y + 0.068F * head_scale * h, 0.022F * head_scale}}, // 38
        {"Lip_Upper", 6, {0.0F, head_y + 0.048F * head_scale * h, 0.105F * head_scale}}, // 39
        {"Lip_Lower", 38, {0.0F, head_y + 0.032F * head_scale * h - jaw_drop * 0.1F, 0.102F * head_scale}}, // 40
        {"Mouth_Corner_L", 6, {0.030F * head_scale, head_y + 0.040F * head_scale * h, 0.096F * head_scale}}, // 41
        {"Mouth_Corner_R", 6, {-0.030F * head_scale, head_y + 0.040F * head_scale * h, 0.096F * head_scale}}, // 42
        {"Eye_L", 6, {0.033F * head_scale, head_y + 0.105F * head_scale * h, 0.085F * head_scale}}, // 43
        {"Eye_R", 6, {-0.033F * head_scale, head_y + 0.105F * head_scale * h, 0.085F * head_scale}}, // 44
        {"Eyelid_Upper_L", 6, {0.033F * head_scale, head_y + 0.118F * head_scale * h, 0.094F * head_scale}}, // 45
        {"Eyelid_Upper_R", 6, {-0.033F * head_scale, head_y + 0.118F * head_scale * h, 0.094F * head_scale}}, // 46
        {"Brow_L", 6, {0.035F * head_scale, head_y + 0.132F * head_scale * h, 0.098F * head_scale}}, // 47
        {"Brow_R", 6, {-0.035F * head_scale, head_y + 0.132F * head_scale * h, 0.098F * head_scale}}, // 48
        {"Brow_Center", 6, {0.0F, head_y + 0.130F * head_scale * h, 0.102F * head_scale}}, // 49
    }};
    bones[31U].model_pos = glm::mix(bones[8U].model_pos, bones[9U].model_pos, 0.50F);
    bones[32U].model_pos = glm::mix(bones[9U].model_pos, bones[10U].model_pos, 0.50F);
    bones[33U].model_pos = glm::mix(bones[16U].model_pos, bones[17U].model_pos, 0.50F);
    bones[34U].model_pos = glm::mix(bones[17U].model_pos, bones[18U].model_pos, 0.50F);
    return bones;
}

// ============================================================================
// MakeHuman / MPFB2 CC0 Human Base Template Loader & Morph Evaluator
// ============================================================================

struct TemplateBoneLocator {
    glm::vec3 offset{0.0F};
    std::vector<std::uint16_t> vertex_indices{};
};

struct TemplateMorphDelta {
    std::uint16_t vid = 0U;
    glm::vec3 delta{0.0F};
};

using TemplateCorner = WardrobeTemplateCorner;

struct TemplatePart {
    std::vector<TemplateCorner> corners{};
    std::vector<std::uint32_t> indices{};
};

enum TemplatePartId : std::size_t {
    part_body = 0U,
    part_eyes = 1U,
    part_tights = 2U,
    part_skirt = 3U,
    part_scalp = 4U,
};

struct HumanBaseTemplate {
    CharacterGender gender = CharacterGender::male;
    std::uint32_t vertex_count = 0U;
    std::uint32_t mask_size = template_mask_size;
    std::vector<glm::vec3> base_positions{};
    std::vector<CookedSkinVertex> skin_weights{};
    std::array<TemplateBoneLocator, character_total_bone_count> bone_locators{};
    std::array<std::vector<TemplateMorphDelta>, template_morph_count> morph_channels{};
    std::array<TemplatePart, template_part_count> parts{};
    std::vector<std::uint8_t> skin_mask_rgba{};
    std::vector<std::uint8_t> skin_detail_rgba{};
    std::array<std::vector<std::uint16_t>, template_landmark_chain_count> landmark_chains{};
};

class BinarySpanReader final {
public:
    explicit BinarySpanReader(const std::span<const std::uint8_t> bytes) noexcept
        : bytes_(bytes) {}

    [[nodiscard]] bool ok() const noexcept { return !failed_; }
    [[nodiscard]] std::size_t remaining() const noexcept {
        return failed_ ? 0U : (bytes_.size() - cursor_);
    }

    [[nodiscard]] bool read_exact(const std::size_t count, std::span<const std::uint8_t>& out) noexcept {
        if (failed_ || count > bytes_.size() - cursor_) {
            failed_ = true;
            return false;
        }
        out = bytes_.subspan(cursor_, count);
        cursor_ += count;
        return true;
    }

    [[nodiscard]] std::uint8_t read_u8() noexcept {
        std::span<const std::uint8_t> s{};
        if (!read_exact(1U, s)) return 0U;
        return s[0];
    }

    [[nodiscard]] std::uint16_t read_u16() noexcept {
        std::span<const std::uint8_t> s{};
        if (!read_exact(2U, s)) return 0U;
        return static_cast<std::uint16_t>(s[0]) |
               (static_cast<std::uint16_t>(s[1]) << 8U);
    }

    [[nodiscard]] std::uint32_t read_u32() noexcept {
        std::span<const std::uint8_t> s{};
        if (!read_exact(4U, s)) return 0U;
        return static_cast<std::uint32_t>(s[0]) |
               (static_cast<std::uint32_t>(s[1]) << 8U) |
               (static_cast<std::uint32_t>(s[2]) << 16U) |
               (static_cast<std::uint32_t>(s[3]) << 24U);
    }

    [[nodiscard]] float read_f32() noexcept {
        return std::bit_cast<float>(read_u32());
    }

private:
    std::span<const std::uint8_t> bytes_{};
    std::size_t cursor_ = 0U;
    bool failed_ = false;
};

[[nodiscard]] std::filesystem::path current_executable_directory() {
#if defined(_WIN32)
    std::array<wchar_t, 2048U> buf{};
    const DWORD len = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (len > 0U && len < buf.size()) {
        return std::filesystem::path(std::wstring_view(buf.data(), len)).parent_path();
    }
#endif
    std::error_code ec;
    return std::filesystem::current_path(ec);
}

[[nodiscard]] std::optional<std::filesystem::path> resolve_human_template_path(
    const CharacterGender gender) {
    const char* filename = (gender == CharacterGender::female)
        ? "human_female.vcrcharbase"
        : "human_male.vcrcharbase";
    const std::filesystem::path rel =
        std::filesystem::path("assets") / "character_templates" / filename;

    auto check_ancestors = [&](std::filesystem::path base) -> std::optional<std::filesystem::path> {
        std::error_code ec;
        for (int depth = 0; depth < 6 && !base.empty(); ++depth) {
            const std::filesystem::path cand = base / rel;
            if (std::filesystem::is_regular_file(cand, ec) && !ec) {
                return cand;
            }
            const std::filesystem::path parent = base.parent_path();
            if (parent == base) break;
            base = parent;
        }
        return std::nullopt;
    };

    if (const auto hit = check_ancestors(current_executable_directory())) {
        return hit;
    }
    if (const auto root = find_engine_workspace_root()) {
        std::error_code ec;
        const std::filesystem::path cand = *root / rel;
        if (std::filesystem::is_regular_file(cand, ec) && !ec) {
            return cand;
        }
    }
    std::error_code ec;
    if (const auto hit = check_ancestors(std::filesystem::current_path(ec))) {
        return hit;
    }
    return std::nullopt;
}

[[nodiscard]] bool load_human_base_template_file(
    const std::filesystem::path& path,
    const CharacterGender expected_gender,
    HumanBaseTemplate& out_tpl) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        return false;
    }
    const std::streamoff file_size_off = in.tellg();
    if (file_size_off < 48 || file_size_off > 64 * 1024 * 1024) {
        return false;
    }
    in.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> raw(static_cast<std::size_t>(file_size_off));
    if (!in.read(reinterpret_cast<char*>(raw.data()), file_size_off)) {
        return false;
    }

    BinarySpanReader rd(raw);
    std::span<const std::uint8_t> magic{};
    if (!rd.read_exact(8U, magic)) return false;
    constexpr std::array<char, 8U> expected_magic{'V', 'C', 'R', 'C', 'B', 'A', 'S', '2'};
    for (std::size_t i = 0U; i < 8U; ++i) {
        if (magic[i] != static_cast<std::uint8_t>(expected_magic[i])) {
            return false;
        }
    }

    const std::uint32_t version = rd.read_u32();
    const std::uint32_t gender_id = rd.read_u32();
    const std::uint32_t vertex_count = rd.read_u32();
    const std::uint32_t morph_count = rd.read_u32();
    const std::uint32_t part_count = rd.read_u32();
    const std::uint32_t mask_size = rd.read_u32();
    const std::uint32_t bone_count = rd.read_u32();
    const std::uint32_t landmark_chain_count = rd.read_u32();
    const std::uint32_t flags = rd.read_u32();
    static_cast<void>(rd.read_u32());

    if (!rd.ok() || version != 2U ||
        gender_id != static_cast<std::uint32_t>(expected_gender) ||
        vertex_count == 0U || vertex_count > 200'000U ||
        morph_count != template_morph_count ||
        part_count != template_part_count ||
        mask_size != template_mask_size ||
        bone_count != character_total_bone_count ||
        landmark_chain_count != template_landmark_chain_count ||
        (flags & 1U) == 0U) {
        return false;
    }

    HumanBaseTemplate tpl{};
    tpl.gender = expected_gender;
    tpl.vertex_count = vertex_count;
    tpl.mask_size = mask_size;
    tpl.base_positions.resize(vertex_count);
    for (std::uint32_t i = 0U; i < vertex_count; ++i) {
        const float x = rd.read_f32();
        const float y = rd.read_f32();
        const float z = rd.read_f32();
        tpl.base_positions[i] = glm::vec3(x, y, z);
    }

    tpl.skin_weights.resize(vertex_count);
    for (std::uint32_t i = 0U; i < vertex_count; ++i) {
        CookedSkinVertex sv{};
        sv.joints[0] = rd.read_u16();
        sv.joints[1] = rd.read_u16();
        sv.joints[2] = rd.read_u16();
        sv.joints[3] = rd.read_u16();
        sv.weights[0] = rd.read_f32();
        sv.weights[1] = rd.read_f32();
        sv.weights[2] = rd.read_f32();
        sv.weights[3] = rd.read_f32();
        sanitize_skin_vertex(sv, character_total_bone_count);
        tpl.skin_weights[i] = sv;
    }

    for (std::uint32_t b = 0U; b < character_total_bone_count; ++b) {
        TemplateBoneLocator& loc = tpl.bone_locators[b];
        loc.offset.x = rd.read_f32();
        loc.offset.y = rd.read_f32();
        loc.offset.z = rd.read_f32();
        const std::uint32_t cnt = rd.read_u32();
        if (!rd.ok() || cnt > 4096U) return false;
        loc.vertex_indices.resize(cnt);
        for (std::uint32_t j = 0U; j < cnt; ++j) {
            const std::uint16_t vid = rd.read_u16();
            if (vid >= vertex_count) return false;
            loc.vertex_indices[j] = vid;
        }
    }

    for (std::uint32_t c = 0U; c < template_morph_count; ++c) {
        const std::uint32_t dcnt = rd.read_u32();
        if (!rd.ok() || dcnt > vertex_count) return false;
        auto& ch = tpl.morph_channels[c];
        ch.resize(dcnt);
        for (std::uint32_t d = 0U; d < dcnt; ++d) {
            const std::uint16_t vid = rd.read_u16();
            static_cast<void>(rd.read_u16());
            const float dx = rd.read_f32();
            const float dy = rd.read_f32();
            const float dz = rd.read_f32();
            if (!rd.ok() || vid >= vertex_count) return false;
            ch[d] = TemplateMorphDelta{vid, glm::vec3(dx, dy, dz)};
        }
    }

    for (std::uint32_t p = 0U; p < template_part_count; ++p) {
        const std::uint32_t ccnt = rd.read_u32();
        const std::uint32_t icnt = rd.read_u32();
        if (!rd.ok() || ccnt > 500'000U || icnt > 2'000'000U) return false;
        TemplatePart& part = tpl.parts[p];
        part.corners.resize(ccnt);
        for (std::uint32_t ci = 0U; ci < ccnt; ++ci) {
            TemplateCorner corner{};
            corner.vid = rd.read_u16();
            corner.region = rd.read_u8();
            corner.flags = rd.read_u8();
            corner.u = rd.read_f32();
            corner.v = rd.read_f32();
            if (!rd.ok() || corner.vid >= vertex_count) return false;
            part.corners[ci] = corner;
        }
        part.indices.resize(icnt);
        for (std::uint32_t ii = 0U; ii < icnt; ++ii) {
            const std::uint32_t idx = rd.read_u32();
            if (!rd.ok() || idx >= ccnt) return false;
            part.indices[ii] = idx;
        }
    }

    const std::size_t mask_bytes =
        static_cast<std::size_t>(mask_size) * static_cast<std::size_t>(mask_size) * 4U;
    std::span<const std::uint8_t> mask_span{};
    if (!rd.read_exact(mask_bytes, mask_span)) return false;
    tpl.skin_mask_rgba.assign(mask_span.begin(), mask_span.end());

    std::span<const std::uint8_t> detail_span{};
    if (!rd.read_exact(mask_bytes, detail_span)) return false;
    tpl.skin_detail_rgba.assign(detail_span.begin(), detail_span.end());

    for (std::uint32_t li = 0U; li < template_landmark_chain_count; ++li) {
        const std::uint16_t cnt = rd.read_u16();
        if (!rd.ok() || cnt > 512U) return false;
        auto& chain = tpl.landmark_chains[li];
        chain.resize(cnt);
        for (std::uint16_t k = 0U; k < cnt; ++k) {
            const std::uint16_t vid = rd.read_u16();
            if (!rd.ok() || vid >= vertex_count) return false;
            chain[k] = vid;
        }
    }
    if (!rd.ok() || rd.remaining() != 0U) return false;

    // Repair swapped Lip_Upper (39) <-> Lip_Lower (40) vertex groups in legacy vcrcharbase templates.
    for (CookedSkinVertex& sv : tpl.skin_weights) {
        for (std::uint16_t& joint_id : sv.joints) {
            if (joint_id == 39U) {
                joint_id = 40U;
            } else if (joint_id == 40U) {
                joint_id = 39U;
            }
        }
    }
    std::swap(tpl.bone_locators[39U], tpl.bone_locators[40U]);

    out_tpl = std::move(tpl);
    return true;
}

[[nodiscard]] const HumanBaseTemplate* get_human_base_template(
    const CharacterGender gender) {
    static std::mutex cache_mutex{};
    static std::array<HumanBaseTemplate, 2U> cache{};
    static std::array<bool, 2U> loaded{};
    static std::array<bool, 2U> attempted{};

    const std::size_t slot = (gender == CharacterGender::female) ? 0U : 1U;
    std::lock_guard<std::mutex> lock(cache_mutex);
    if (loaded[slot]) {
        return &cache[slot];
    }
    if (attempted[slot]) {
        return nullptr;
    }
    attempted[slot] = true;
    const auto path = resolve_human_template_path(gender);
    if (!path) {
        return nullptr;
    }
    HumanBaseTemplate tpl{};
    if (!load_human_base_template_file(*path, gender, tpl)) {
        return nullptr;
    }
    cache[slot] = std::move(tpl);
    loaded[slot] = true;
    return &cache[slot];
}

struct EvaluatedHumanTemplate {
    std::vector<glm::vec3> positions{};
    std::array<BoneModelSpec, character_total_bone_count> bones{};
};

[[nodiscard]] float sum_bone_weights(
    const CookedSkinVertex& sv,
    const std::span<const std::uint16_t> bone_ids) noexcept {
    float total = 0.0F;
    for (std::size_t k = 0U; k < 4U; ++k) {
        for (const std::uint16_t bid : bone_ids) {
            if (sv.joints[k] == bid) {
                total += sv.weights[k];
                break;
            }
        }
    }
    return total;
}

[[nodiscard]] EvaluatedHumanTemplate evaluate_human_template(
    const CharacterBodyParams& body,
    const HumanBaseTemplate& tpl) {
    EvaluatedHumanTemplate eval{};
    eval.positions = tpl.base_positions;
    const std::size_t vcount = eval.positions.size();

    float sw = body.shoulder_width;
    float cd = body.chest_depth;
    float ww = body.waist_width;
    float hw = body.hip_width;
    float musc = body.musculature;
    float fat = body.body_fat;
    float jaw_w = body.face.jaw_width;
    float chin_l = body.face.chin_length;
    float cheek_p = body.face.cheekbone_prominence;
    float brow_r = body.face.brow_ridge;

    switch (body.archetype) {
    case CharacterArchetype::human_balanced:
        break;
    case CharacterArchetype::human_athletic:
        musc = std::clamp(musc + 0.22F, 0.0F, 1.0F);
        sw = std::clamp(sw + 0.12F, 0.0F, 1.0F);
        ww = std::clamp(ww - 0.08F, 0.0F, 1.0F);
        fat = std::clamp(fat - 0.08F, 0.0F, 1.0F);
        break;
    case CharacterArchetype::human_heavy:
        musc = std::clamp(musc + 0.18F, 0.0F, 1.0F);
        fat = std::clamp(fat + 0.35F, 0.0F, 1.0F);
        sw = std::clamp(sw + 0.16F, 0.0F, 1.0F);
        cd = std::clamp(cd + 0.20F, 0.0F, 1.0F);
        ww = std::clamp(ww + 0.26F, 0.0F, 1.0F);
        hw = std::clamp(hw + 0.18F, 0.0F, 1.0F);
        jaw_w = std::clamp(jaw_w + 0.14F, 0.0F, 1.0F);
        break;
    case CharacterArchetype::human_slender:
        musc = std::clamp(musc - 0.16F, 0.0F, 1.0F);
        fat = std::clamp(fat - 0.14F, 0.0F, 1.0F);
        sw = std::clamp(sw - 0.10F, 0.0F, 1.0F);
        ww = std::clamp(ww - 0.12F, 0.0F, 1.0F);
        hw = std::clamp(hw - 0.06F, 0.0F, 1.0F);
        break;
    case CharacterArchetype::stylized_hero:
        musc = std::clamp(musc + 0.26F, 0.0F, 1.0F);
        sw = std::clamp(sw + 0.22F, 0.0F, 1.0F);
        ww = std::clamp(ww - 0.12F, 0.0F, 1.0F);
        chin_l = std::clamp(chin_l + 0.12F, 0.0F, 1.0F);
        brow_r = std::clamp(brow_r + 0.10F, 0.0F, 1.0F);
        break;
    case CharacterArchetype::veteran_wanderer:
        fat = std::clamp(fat - 0.05F, 0.0F, 1.0F);
        musc = std::clamp(musc + 0.08F, 0.0F, 1.0F);
        cheek_p = std::clamp(cheek_p + 0.12F, 0.0F, 1.0F);
        brow_r = std::clamp(brow_r + 0.14F, 0.0F, 1.0F);
        break;
    }

    const std::array<float, template_morph_count> weights{{
        std::max(0.0F, (musc - 0.45F) / 0.55F),          // 0: musculature_pos
        std::max(0.0F, (0.45F - musc) / 0.45F),          // 1: musculature_neg
        std::max(0.0F, (fat - 0.20F) / 0.80F),           // 2: body_fat_pos
        std::max(0.0F, (0.20F - fat) / 0.20F),           // 3: body_fat_neg
        (sw - 0.50F) * 2.0F,                             // 4: shoulder_width
        (cd - 0.50F) * 2.0F,                             // 5: chest_depth
        (ww - 0.50F) * 2.0F,                             // 6: waist_width
        (hw - 0.50F) * 2.0F,                             // 7: hip_width
        (jaw_w - 0.50F) * 2.2F,                          // 8: jaw_width
        (chin_l - 0.50F) * 2.2F,                         // 9: chin_length
        (cheek_p - 0.50F) * 2.2F,                        // 10: cheekbone_prominence
        (brow_r - 0.45F) * 2.2F,                         // 11: brow_ridge
        (body.face.nose_length - 0.50F) * 2.2F,          // 12: nose_length
        (body.face.nose_width - 0.45F) * 2.2F,           // 13: nose_width
        (body.face.eye_size - 0.55F) * 2.0F,             // 14: eye_size
        (body.face.eye_spacing - 0.50F) * 2.0F,          // 15: eye_spacing
        (body.face.mouth_width - 0.50F) * 2.2F,          // 16: mouth_width
        (body.face.lip_fullness - 0.50F) * 2.2F,         // 17: lip_fullness
    }};

    for (std::size_t ch = 0U; ch < template_morph_count; ++ch) {
        const float w = weights[ch];
        if (std::abs(w) < 1.0e-4F) continue;
        for (const TemplateMorphDelta& md : tpl.morph_channels[ch]) {
            if (md.vid < vcount) {
                eval.positions[md.vid] += md.delta * w;
            }
        }
    }

    const auto locator_mean = [&](const std::size_t bone_idx) noexcept -> glm::vec3 {
        const TemplateBoneLocator& loc = tpl.bone_locators[bone_idx];
        if (loc.vertex_indices.empty()) {
            return glm::vec3(0.0F);
        }
        glm::vec3 sum(0.0F);
        for (const std::uint16_t vi : loc.vertex_indices) {
            sum += eval.positions[vi];
        }
        return sum / static_cast<float>(loc.vertex_indices.size());
    };

    const glm::vec3 neck_pivot = locator_mean(5U) + glm::vec3(0.0F, 0.02F, 0.0F);
    const glm::vec3 pelvis_pivot = locator_mean(1U);
    const glm::vec3 shld_l_pivot = locator_mean(8U);
    const glm::vec3 shld_r_pivot = locator_mean(16U);

    const float head_scale = body.face.head_scale;
    const float arm_mul = 0.86F + body.arm_length * 0.28F;
    const float leg_mul = 0.86F + body.leg_length * 0.28F;

    constexpr std::array<std::uint16_t, 13U> head_bones{{
        6U, 38U, 39U, 40U, 41U, 42U, 43U, 44U, 45U, 46U, 47U, 48U, 49U}};
    constexpr std::array<std::uint16_t, 1U> neck_bones{{5U}};
    constexpr std::array<std::uint16_t, 9U> arm_l_bones{{
        8U, 31U, 9U, 32U, 10U, 11U, 12U, 13U, 14U}};
    constexpr std::array<std::uint16_t, 9U> arm_r_bones{{
        16U, 33U, 17U, 34U, 18U, 19U, 20U, 21U, 22U}};

    for (std::size_t vi = 0U; vi < vcount; ++vi) {
        const CookedSkinVertex& sv = tpl.skin_weights[vi];
        glm::vec3 p = eval.positions[vi];

        if (std::abs(head_scale - 1.0F) > 1.0e-4F) {
            const float wh = std::clamp(
                sum_bone_weights(sv, head_bones) + 0.50F * sum_bone_weights(sv, neck_bones),
                0.0F, 1.0F);
            if (wh > 1.0e-4F) {
                const float s = 1.0F + (head_scale - 1.0F) * wh;
                p = neck_pivot + (p - neck_pivot) * s;
            }
        }
        if (std::abs(arm_mul - 1.0F) > 1.0e-4F) {
            const float wl = std::clamp(sum_bone_weights(sv, arm_l_bones), 0.0F, 1.0F);
            if (wl > 1.0e-4F) {
                p += (p - shld_l_pivot) * ((arm_mul - 1.0F) * wl);
            }
            const float wr = std::clamp(sum_bone_weights(sv, arm_r_bones), 0.0F, 1.0F);
            if (wr > 1.0e-4F) {
                p += (p - shld_r_pivot) * ((arm_mul - 1.0F) * wr);
            }
        }
        if (std::abs(leg_mul - 1.0F) > 1.0e-4F && pelvis_pivot.y > 0.1F) {
            const float ref_y = std::clamp(p.y, 0.0F, pelvis_pivot.y);
            p.y += (leg_mul - 1.0F) * ref_y;
        }

        eval.positions[vi] = p;
    }

    float min_body_y = std::numeric_limits<float>::max();
    float max_body_y = std::numeric_limits<float>::lowest();
    for (const TemplateCorner& c : tpl.parts[part_body].corners) {
        const float y = eval.positions[c.vid].y;
        min_body_y = (std::min)(min_body_y, y);
        max_body_y = (std::max)(max_body_y, y);
    }
    const float raw_height = (std::max)(0.5F, max_body_y - min_body_y);
    const float height_scale = body.height_meters / raw_height;
    for (glm::vec3& p : eval.positions) {
        p.x *= height_scale;
        p.y = (p.y - min_body_y) * height_scale;
        p.z *= height_scale;
    }

    const auto& fallback = fallback_character_bone_layout(body);
    for (std::size_t b = 0U; b < character_total_bone_count; ++b) {
        eval.bones[b].name = fallback[b].name;
        eval.bones[b].parent = fallback[b].parent;
        if (b == 0U) {
            eval.bones[b].model_pos = glm::vec3(0.0F);
        } else if (tpl.bone_locators[b].vertex_indices.empty()) {
            eval.bones[b].model_pos = fallback[b].model_pos;
        } else {
            eval.bones[b].model_pos =
                locator_mean(b) + tpl.bone_locators[b].offset * height_scale;
        }
    }

    // Enforce exact collinear midpoints for upper-arm and forearm twist pivots.
    eval.bones[31U].model_pos =
        glm::mix(eval.bones[8U].model_pos, eval.bones[9U].model_pos, 0.50F);
    eval.bones[32U].model_pos =
        glm::mix(eval.bones[9U].model_pos, eval.bones[10U].model_pos, 0.50F);
    eval.bones[33U].model_pos =
        glm::mix(eval.bones[16U].model_pos, eval.bones[17U].model_pos, 0.50F);
    eval.bones[34U].model_pos =
        glm::mix(eval.bones[17U].model_pos, eval.bones[18U].model_pos, 0.50F);

    // Anchor Jaw (38) posteriorly at the anatomical TMJ condyle hinge.
    const glm::vec3 eye_mid =
        0.5F * (eval.bones[43U].model_pos + eval.bones[44U].model_pos);
    const glm::vec3 corner_mid =
        0.5F * (eval.bones[41U].model_pos + eval.bones[42U].model_pos);
    eval.bones[38U].model_pos = glm::vec3(
        0.0F,
        glm::mix(corner_mid.y, eye_mid.y, 0.55F),
        eval.bones[6U].model_pos.z + 0.22F * (eye_mid.z - eval.bones[6U].model_pos.z));

    return eval;
}

[[nodiscard]] std::array<BoneModelSpec, character_total_bone_count>
compute_character_bone_layout(const CharacterBodyParams& body) noexcept {
    if (const HumanBaseTemplate* tpl = get_human_base_template(body.gender)) {
        return evaluate_human_template(body, *tpl).bones;
    }
    return fallback_character_bone_layout(body);
}

// ============================================================================
// Welded Mesh Assembly, Wrapped Garment Rims & Sculpting
// ============================================================================

struct MeshBuilder {
    std::vector<CookedModelVertex> vertices{};
    std::vector<CookedSkinVertex> skin_vertices{};
    std::vector<std::uint8_t> vertex_paint_targets{};
    std::vector<std::uint8_t> vertex_garment_zones{};
    std::vector<float> vertex_garment_ao{};
    std::vector<float> vertex_garment_curv{};
    std::vector<std::uint32_t> indices{};
    std::vector<CharacterSubmeshBuild> submeshes{};

    void add_triangle_vertex(
        const glm::vec3& pos,
        const glm::vec3& norm,
        const glm::vec2& uv,
        const CookedSkinVertex& skin,
        const CharacterPaintTarget target,
        std::uint32_t& out_index) {
        CookedModelVertex v{};
        v.position = {pos.x, pos.y, pos.z};
        const float nlen = glm::length(norm);
        const glm::vec3 nn = nlen > 1.0e-5F ? (norm / nlen) : glm::vec3(0.0F, 1.0F, 0.0F);
        v.normal = {nn.x, nn.y, nn.z};
        v.texcoord = {uv.x, 1.0F - uv.y};
        v.tangent = {1.0F, 0.0F, 0.0F, 1.0F};
        out_index = static_cast<std::uint32_t>(vertices.size());
        vertices.push_back(v);
        CookedSkinVertex sanitized = skin;
        sanitize_skin_vertex(sanitized, character_total_bone_count);
        skin_vertices.push_back(sanitized);
        vertex_paint_targets.push_back(static_cast<std::uint8_t>(target));
        vertex_garment_zones.push_back(static_cast<std::uint8_t>(GarmentZoneId::none));
        vertex_garment_ao.push_back(1.0F);
        vertex_garment_curv.push_back(0.5F);
    }
};

struct LoftRing {
    glm::vec3 center{0.0F};
    glm::vec3 axis_x{1.0F, 0.0F, 0.0F};
    glm::vec3 axis_z{0.0F, 0.0F, 1.0F};
    float radius_x = 0.1F;
    float radius_z = 0.1F;
    float v_coord = 0.0F;
    CookedSkinVertex skin{};
};

void emit_loft_tube(
    MeshBuilder& mb,
    std::span<const LoftRing> rings,
    const std::uint32_t radial_segments,
    const glm::vec2 uv_min,
    const glm::vec2 uv_max,
    const CharacterPaintTarget target,
    const bool close_bottom_cap,
    const bool close_top_cap,
    const float boxiness = 0.0F) {
    if (rings.size() < 2U || radial_segments < 3U) {
        return;
    }
    const std::uint32_t cols = radial_segments + 1U;
    const auto base_vertex = static_cast<std::uint32_t>(mb.vertices.size());
    for (std::size_t r = 0U; r < rings.size(); ++r) {
        const LoftRing& ring = rings[r];
        const float v_uv = uv_min.y + ring.v_coord * (uv_max.y - uv_min.y);
        for (std::uint32_t c = 0U; c < cols; ++c) {
            const float frac = static_cast<float>(c) / static_cast<float>(radial_segments);
            const float angle = frac * glm::two_pi<float>();
            float cx = std::sin(angle);
            float cz = std::cos(angle);
            if (boxiness > 0.0F) {
                const float mx = (std::max)(std::abs(cx), std::abs(cz));
                if (mx > 1.0e-4F) {
                    const float sx = cx / mx;
                    const float sz = cz / mx;
                    cx = cx + (sx - cx) * boxiness;
                    cz = cz + (sz - cz) * boxiness;
                }
            }
            const glm::vec3 radial_dir =
                ring.axis_x * (cx * ring.radius_x) +
                ring.axis_z * (cz * ring.radius_z);
            const glm::vec3 pos = ring.center + radial_dir;
            const glm::vec3 nrm = ring.axis_x * cx + ring.axis_z * cz;
            const float u_uv = uv_min.x + frac * (uv_max.x - uv_min.x);
            std::uint32_t out_idx = 0U;
            mb.add_triangle_vertex(pos, nrm, {u_uv, v_uv}, ring.skin, target, out_idx);
        }
    }

    for (std::size_t r = 0U; r + 1U < rings.size(); ++r) {
        for (std::uint32_t c = 0U; c < radial_segments; ++c) {
            const std::uint32_t i00 = base_vertex + static_cast<std::uint32_t>(r) * cols + c;
            const std::uint32_t i10 = i00 + 1U;
            const std::uint32_t i01 = i00 + cols;
            const std::uint32_t i11 = i01 + 1U;
            mb.indices.push_back(i00);
            mb.indices.push_back(i01);
            mb.indices.push_back(i11);
            mb.indices.push_back(i00);
            mb.indices.push_back(i11);
            mb.indices.push_back(i10);
        }
    }

    if (close_bottom_cap) {
        std::uint32_t center_idx = 0U;
        const glm::vec3 axis_y = glm::normalize(
            glm::cross(rings.front().axis_z, rings.front().axis_x));
        mb.add_triangle_vertex(
            rings.front().center, -axis_y, 0.5F * (uv_min + uv_max),
            rings.front().skin, target, center_idx);
        for (std::uint32_t c = 0U; c < radial_segments; ++c) {
            mb.indices.push_back(center_idx);
            mb.indices.push_back(base_vertex + c + 1U);
            mb.indices.push_back(base_vertex + c);
        }
    }
    if (close_top_cap) {
        std::uint32_t center_idx = 0U;
        const glm::vec3 axis_y = glm::normalize(
            glm::cross(rings.back().axis_z, rings.back().axis_x));
        mb.add_triangle_vertex(
            rings.back().center, axis_y, 0.5F * (uv_min + uv_max),
            rings.back().skin, target, center_idx);
        const std::uint32_t top_row =
            base_vertex + static_cast<std::uint32_t>(rings.size() - 1U) * cols;
        for (std::uint32_t c = 0U; c < radial_segments; ++c) {
            mb.indices.push_back(center_idx);
            mb.indices.push_back(top_row + c);
            mb.indices.push_back(top_row + c + 1U);
        }
    }
}

void emit_ellipsoid(
    MeshBuilder& mb,
    const glm::vec3& center,
    const glm::vec3& radii,
    const std::uint32_t stacks,
    const std::uint32_t slices,
    const glm::vec2 uv_min,
    const glm::vec2 uv_max,
    const CookedSkinVertex& skin,
    const CharacterPaintTarget target) {
    const auto base = static_cast<std::uint32_t>(mb.vertices.size());
    const std::uint32_t cols = slices + 1U;
    for (std::uint32_t st = 0U; st <= stacks; ++st) {
        const float v = static_cast<float>(st) / static_cast<float>(stacks);
        const float phi = v * glm::pi<float>();
        const float sp = std::sin(phi);
        const float cp = -std::cos(phi);
        for (std::uint32_t sl = 0U; sl <= slices; ++sl) {
            const float u = static_cast<float>(sl) / static_cast<float>(slices);
            const float theta = u * glm::two_pi<float>();
            const glm::vec3 dir(sp * std::sin(theta), cp, sp * std::cos(theta));
            const glm::vec3 pos = center + dir * radii;
            const glm::vec2 uv(
                uv_min.x + u * (uv_max.x - uv_min.x),
                uv_min.y + v * (uv_max.y - uv_min.y));
            std::uint32_t idx = 0U;
            mb.add_triangle_vertex(pos, dir, uv, skin, target, idx);
        }
    }
    for (std::uint32_t st = 0U; st < stacks; ++st) {
        for (std::uint32_t sl = 0U; sl < slices; ++sl) {
            const std::uint32_t i00 = base + st * cols + sl;
            const std::uint32_t i10 = i00 + 1U;
            const std::uint32_t i01 = i00 + cols;
            const std::uint32_t i11 = i01 + 1U;
            mb.indices.push_back(i00);
            mb.indices.push_back(i01);
            mb.indices.push_back(i11);
            mb.indices.push_back(i00);
            mb.indices.push_back(i11);
            mb.indices.push_back(i10);
        }
    }
}

[[nodiscard]] std::vector<glm::vec3> compute_part_welded_normals(
    const TemplatePart& part,
    const std::span<const glm::vec3> welded_positions) {
    std::vector<glm::vec3> accum(welded_positions.size(), glm::vec3(0.0F));
    for (std::size_t i = 0U; i + 2U < part.indices.size(); i += 3U) {
        const std::uint16_t v0 = part.corners[part.indices[i]].vid;
        const std::uint16_t v1 = part.corners[part.indices[i + 1U]].vid;
        const std::uint16_t v2 = part.corners[part.indices[i + 2U]].vid;
        const glm::vec3 face_n = glm::cross(
            welded_positions[v1] - welded_positions[v0],
            welded_positions[v2] - welded_positions[v0]);
        accum[v0] += face_n;
        accum[v1] += face_n;
        accum[v2] += face_n;
    }
    for (glm::vec3& n : accum) {
        const float len = glm::length(n);
        n = len > 1.0e-7F ? (n / len) : glm::vec3(0.0F, 1.0F, 0.0F);
    }
    return accum;
}

[[nodiscard]] glm::vec4 sample_rgba_buffer_bilinear(
    const std::span<const std::uint8_t> buffer,
    const std::uint32_t dim,
    const float u,
    const float v) noexcept {
    if (dim == 0U ||
        buffer.size() < static_cast<std::size_t>(dim) * static_cast<std::size_t>(dim) * 4U) {
        return glm::vec4(0.0F);
    }
    const std::uint32_t max_coord = dim - 1U;
    const float fx = std::clamp(u, 0.0F, 1.0F) * static_cast<float>(max_coord);
    const float fy = std::clamp(v, 0.0F, 1.0F) * static_cast<float>(max_coord);
    const auto x0 = static_cast<std::uint32_t>(fx);
    const auto y0 = static_cast<std::uint32_t>(fy);
    const std::uint32_t x1 = (std::min)(x0 + 1U, max_coord);
    const std::uint32_t y1 = (std::min)(y0 + 1U, max_coord);
    const float tx = fx - static_cast<float>(x0);
    const float ty = fy - static_cast<float>(y0);

    const auto fetch = [&](const std::uint32_t x, const std::uint32_t y) noexcept {
        const std::size_t off = (static_cast<std::size_t>(y) * dim + x) * 4U;
        constexpr float inv255 = 1.0F / 255.0F;
        return glm::vec4(
            static_cast<float>(buffer[off + 0U]) * inv255,
            static_cast<float>(buffer[off + 1U]) * inv255,
            static_cast<float>(buffer[off + 2U]) * inv255,
            static_cast<float>(buffer[off + 3U]) * inv255);
    };
    const glm::vec4 c0 = glm::mix(fetch(x0, y0), fetch(x1, y0), tx);
    const glm::vec4 c1 = glm::mix(fetch(x0, y1), fetch(x1, y1), tx);
    return glm::mix(c0, c1, ty);
}

[[nodiscard]] glm::vec4 sample_skin_mask_bilinear(
    const HumanBaseTemplate& tpl,
    const float u,
    const float v) noexcept {
    return sample_rgba_buffer_bilinear(tpl.skin_mask_rgba, tpl.mask_size, u, v);
}

[[nodiscard]] glm::vec4 sample_skin_detail_bilinear(
    const HumanBaseTemplate& tpl,
    const float u,
    const float v) noexcept {
    return sample_rgba_buffer_bilinear(tpl.skin_detail_rgba, tpl.mask_size, u, v);
}

void apply_sculpt_strokes_to_mesh(
    const std::span<const SculptStrokeRecord> strokes,
    std::vector<CookedModelVertex>& vertices,
    const std::size_t skin_vertex_limit) {
    if (strokes.empty() || skin_vertex_limit == 0U) {
        return;
    }
    const std::size_t count = (std::min)(skin_vertex_limit, vertices.size());
    std::vector<glm::vec3> scratch;
    scratch.resize(count);

    for (const SculptStrokeRecord& stroke : strokes) {
        const float r = std::clamp(stroke.radius, 0.015F, 0.65F);
        const float r2 = r * r;
        const float s = std::clamp(stroke.strength, 0.01F, 1.0F);
        const int passes = stroke.mirror_x ? 2 : 1;

        if (stroke.brush == SculptBrushKind::smooth) {
            for (int p = 0; p < passes; ++p) {
                const glm::vec3 center = (p == 1)
                    ? glm::vec3(-stroke.local_center.x, stroke.local_center.y, stroke.local_center.z)
                    : stroke.local_center;
                glm::vec3 weighted_avg(0.0F);
                float total_w = 0.0F;
                for (std::size_t i = 0U; i < count; ++i) {
                    const glm::vec3 pos(vertices[i].position[0], vertices[i].position[1], vertices[i].position[2]);
                    const float d2 = glm::dot(pos - center, pos - center);
                    if (d2 < r2) {
                        const float falloff = 1.0F - std::sqrt(d2) / r;
                        weighted_avg += pos * falloff;
                        total_w += falloff;
                    }
                    scratch[i] = pos;
                }
                if (total_w > 1.0e-5F) {
                    const glm::vec3 target = weighted_avg / total_w;
                    for (std::size_t i = 0U; i < count; ++i) {
                        const float d2 = glm::dot(scratch[i] - center, scratch[i] - center);
                        if (d2 < r2) {
                            const float w = std::pow(1.0F - std::sqrt(d2) / r, 1.5F) * s * 0.45F;
                            const glm::vec3 next = glm::mix(scratch[i], target, w);
                            vertices[i].position = {next.x, next.y, next.z};
                        }
                    }
                }
            }
            continue;
        }

        for (int p = 0; p < passes; ++p) {
            const glm::vec3 center = (p == 1)
                ? glm::vec3(-stroke.local_center.x, stroke.local_center.y, stroke.local_center.z)
                : stroke.local_center;
            const glm::vec3 grab_disp = (p == 1)
                ? glm::vec3(-stroke.displacement.x, stroke.displacement.y, stroke.displacement.z)
                : stroke.displacement;
            for (std::size_t i = 0U; i < count; ++i) {
                glm::vec3 pos(vertices[i].position[0], vertices[i].position[1], vertices[i].position[2]);
                const float d2 = glm::dot(pos - center, pos - center);
                if (d2 >= r2) {
                    continue;
                }
                const float t = 1.0F - std::sqrt(d2) / r;
                const float falloff = t * t * (3.0F - 2.0F * t) * s;
                const glm::vec3 nrm(vertices[i].normal[0], vertices[i].normal[1], vertices[i].normal[2]);
                if (stroke.brush == SculptBrushKind::inflate) {
                    pos += nrm * (0.025F * falloff);
                } else if (stroke.brush == SculptBrushKind::deflate) {
                    pos -= nrm * (0.025F * falloff);
                } else if (stroke.brush == SculptBrushKind::grab) {
                    pos += grab_disp * falloff;
                } else if (stroke.brush == SculptBrushKind::flatten) {
                    const glm::vec3 avg_n = glm::length(grab_disp) > 1.0e-5F
                        ? glm::normalize(grab_disp)
                        : nrm;
                    const float plane_dist = glm::dot(pos - center, avg_n);
                    pos -= avg_n * (plane_dist * 0.6F * falloff);
                }
                vertices[i].position = {pos.x, pos.y, pos.z};
            }
        }
    }
}

[[maybe_unused]] [[nodiscard]] PbrMaterial preset_outfit_material(
    const OutfitMaterialPreset preset,
    const glm::vec3& color) noexcept {
    return make_wardrobe_preset_material(preset, color);
}

// ============================================================================
// Formant Speech & UTF-8 Text-to-Viseme Engine
// ============================================================================

struct PhonemeFrame {
    float duration_s = 0.075F;
    float f1 = 500.0F;
    float f2 = 1500.0F;
    float f3 = 2500.0F;
    float voiced = 1.0F;
    float fricative = 0.0F;
    float jaw_open = 0.25F;
    float lip_round = 0.0F;
    float lip_wide = 0.1F;
    float lip_pucker = 0.0F;
    float upper_lip = 0.1F;
};

[[nodiscard]] std::uint32_t next_utf8_codepoint(
    const std::string_view text,
    std::size_t& offset) noexcept {
    if (offset >= text.size()) {
        return 0U;
    }
    const auto b0 = static_cast<unsigned char>(text[offset++]);
    if (b0 < 0x80U) {
        return b0;
    }
    if ((b0 & 0xE0U) == 0xC0U && offset < text.size()) {
        const auto b1 = static_cast<unsigned char>(text[offset++]);
        return ((b0 & 0x1FU) << 6U) | (b1 & 0x3FU);
    }
    if ((b0 & 0xF0U) == 0xE0U && offset + 1U < text.size()) {
        const auto b1 = static_cast<unsigned char>(text[offset++]);
        const auto b2 = static_cast<unsigned char>(text[offset++]);
        return ((b0 & 0x0FU) << 12U) | ((b1 & 0x3FU) << 6U) | (b2 & 0x3FU);
    }
    if ((b0 & 0xF8U) == 0xF0U && offset + 2U < text.size()) {
        const auto b1 = static_cast<unsigned char>(text[offset++]);
        const auto b2 = static_cast<unsigned char>(text[offset++]);
        const auto b3 = static_cast<unsigned char>(text[offset++]);
        return ((b0 & 0x07U) << 18U) | ((b1 & 0x3FU) << 12U) |
               ((b2 & 0x3FU) << 6U) | (b3 & 0x3FU);
    }
    return b0;
}

[[nodiscard]] PhonemeFrame classify_codepoint(
    const std::uint32_t cp,
    const float rate_scale) noexcept {
    PhonemeFrame p{};
    const float inv_rate = 1.0F / std::clamp(rate_scale, 0.55F, 1.80F);

    // Lowercase ASCII or Russian Cyrillic to canonical codepoints
    std::uint32_t c = cp;
    if (c >= 'A' && c <= 'Z') {
        c = c - 'A' + 'a';
    } else if (c >= 0x0410U && c <= 0x042FU) {
        c += 0x20U; // А..Я -> а..я
    } else if (c == 0x0401U) {
        c = 0x0451U; // Ё -> ё
    }

    // Open A (a / а / я)
    if (c == 'a' || c == 0x0430U || c == 0x044FU) {
        p = {0.105F * inv_rate, 740.0F, 1280.0F, 2500.0F, 1.0F, 0.04F, 0.78F, 0.08F, 0.25F, 0.0F, 0.25F};
        return p;
    }
    // Wide E/I (e, i, и, е, э)
    if (c == 'e' || c == 'i' || c == 'y' ||
        c == 0x0438U || c == 0x0435U || c == 0x044DU || c == 0x0439U) {
        p = {0.095F * inv_rate, 340.0F, 2150.0F, 2850.0F, 1.0F, 0.05F, 0.36F, 0.0F, 0.85F, 0.0F, 0.30F};
        return p;
    }
    // Round O (o, о, ё)
    if (c == 'o' || c == 0x043EU || c == 0x0451U) {
        p = {0.105F * inv_rate, 480.0F, 920.0F, 2400.0F, 1.0F, 0.04F, 0.56F, 0.78F, 0.0F, 0.45F, 0.15F};
        return p;
    }
    // Tight U (u, у, ю)
    if (c == 'u' || c == 'w' || c == 0x0443U || c == 0x044EU) {
        p = {0.100F * inv_rate, 320.0F, 760.0F, 2250.0F, 1.0F, 0.04F, 0.30F, 0.92F, 0.0F, 0.85F, 0.12F};
        return p;
    }
    // Bilabial (m, b, p, м, б, п) -> closed lips
    if (c == 'm' || c == 'b' || c == 'p' ||
        c == 0x043CU || c == 0x0431U || c == 0x043FU) {
        p = {0.068F * inv_rate, 280.0F, 980.0F, 2200.0F, 0.55F, 0.05F, 0.02F, 0.05F, 0.05F, 0.35F, 0.0F};
        return p;
    }
    // Labiodental (f, v, ф, в)
    if (c == 'f' || c == 'v' || c == 0x0444U || c == 0x0432U) {
        p = {0.072F * inv_rate, 360.0F, 1150.0F, 2400.0F, 0.35F, 0.55F, 0.12F, 0.05F, 0.20F, 0.15F, 0.45F};
        return p;
    }
    // Fricatives (s, z, c, sh, с, з, ш, ж, щ, ч, ц, х)
    if (c == 's' || c == 'z' || c == 'h' ||
        c == 0x0441U || c == 0x0437U || c == 0x0448U || c == 0x0436U ||
        c == 0x0449U || c == 0x0447U || c == 0x0446U || c == 0x0445U) {
        p = {0.070F * inv_rate, 390.0F, 1850.0F, 2700.0F, 0.25F, 0.72F, 0.18F, 0.0F, 0.45F, 0.10F, 0.28F};
        return p;
    }
    // Liquid / nasal / stop consonants
    if ((c >= 'a' && c <= 'z') || (c >= 0x0430U && c <= 0x044FU) || (c >= '0' && c <= '9')) {
        p = {0.065F * inv_rate, 440.0F, 1450.0F, 2450.0F, 0.75F, 0.18F, 0.32F, 0.18F, 0.22F, 0.05F, 0.12F};
        return p;
    }
    // Punctuation pauses
    if (c == ',' || c == ';') {
        p = {0.140F * inv_rate, 350.0F, 1200.0F, 2200.0F, 0.0F, 0.0F, 0.05F, 0.0F, 0.0F, 0.0F, 0.0F};
        return p;
    }
    if (c == '.' || c == '!' || c == '?') {
        p = {0.220F * inv_rate, 350.0F, 1200.0F, 2200.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F};
        return p;
    }
    // Whitespace
    p = {0.045F * inv_rate, 420.0F, 1300.0F, 2350.0F, 0.0F, 0.0F, 0.10F, 0.05F, 0.05F, 0.0F, 0.05F};
    return p;
}

} // namespace

// ============================================================================
// Token Conversions
// ============================================================================

std::string_view to_token(const CharacterRole role) noexcept {
    switch (role) {
    case CharacterRole::npc: return "npc";
    case CharacterRole::player: return "player";
    case CharacterRole::companion: return "companion";
    case CharacterRole::vendor: return "vendor";
    case CharacterRole::boss: return "boss";
    }
    return "npc";
}

std::optional<CharacterRole> parse_character_role(const std::string_view token) noexcept {
    if (token == "npc") return CharacterRole::npc;
    if (token == "player") return CharacterRole::player;
    if (token == "companion") return CharacterRole::companion;
    if (token == "vendor") return CharacterRole::vendor;
    if (token == "boss") return CharacterRole::boss;
    return std::nullopt;
}

std::string_view to_token(const CharacterGender gender) noexcept {
    switch (gender) {
    case CharacterGender::female: return "female";
    case CharacterGender::male: return "male";
    }
    return "male";
}

std::optional<CharacterGender> parse_character_gender(const std::string_view token) noexcept {
    if (token == "female" || token == "Female" || token == "FEMALE" || token == "f" || token == "F") {
        return CharacterGender::female;
    }
    if (token == "male" || token == "Male" || token == "MALE" || token == "m" || token == "M") {
        return CharacterGender::male;
    }
    return std::nullopt;
}

std::string_view to_token(const CharacterArchetype archetype) noexcept {
    switch (archetype) {
    case CharacterArchetype::human_balanced: return "human_balanced";
    case CharacterArchetype::human_athletic: return "human_athletic";
    case CharacterArchetype::human_heavy: return "human_heavy";
    case CharacterArchetype::human_slender: return "human_slender";
    case CharacterArchetype::stylized_hero: return "stylized_hero";
    case CharacterArchetype::veteran_wanderer: return "veteran_wanderer";
    }
    return "human_balanced";
}

std::optional<CharacterArchetype> parse_character_archetype(const std::string_view token) noexcept {
    if (token == "human_balanced") return CharacterArchetype::human_balanced;
    if (token == "human_athletic") return CharacterArchetype::human_athletic;
    if (token == "human_heavy") return CharacterArchetype::human_heavy;
    if (token == "human_slender") return CharacterArchetype::human_slender;
    if (token == "stylized_hero") return CharacterArchetype::stylized_hero;
    if (token == "veteran_wanderer") return CharacterArchetype::veteran_wanderer;
    return std::nullopt;
}

std::string_view to_token(const SculptBrushKind brush) noexcept {
    switch (brush) {
    case SculptBrushKind::inflate: return "inflate";
    case SculptBrushKind::deflate: return "deflate";
    case SculptBrushKind::smooth: return "smooth";
    case SculptBrushKind::grab: return "grab";
    case SculptBrushKind::flatten: return "flatten";
    }
    return "inflate";
}

std::optional<SculptBrushKind> parse_sculpt_brush_kind(const std::string_view token) noexcept {
    if (token == "inflate") return SculptBrushKind::inflate;
    if (token == "deflate") return SculptBrushKind::deflate;
    if (token == "smooth") return SculptBrushKind::smooth;
    if (token == "grab") return SculptBrushKind::grab;
    if (token == "flatten") return SculptBrushKind::flatten;
    return std::nullopt;
}

std::string_view to_token(const TopGarmentStyle style) noexcept {
    switch (style) {
    case TopGarmentStyle::none: return "none";
    case TopGarmentStyle::tunic: return "tunic";
    case TopGarmentStyle::jacket: return "jacket";
    case TopGarmentStyle::trench_coat: return "trench_coat";
    case TopGarmentStyle::armor_cuirass: return "armor_cuirass";
    case TopGarmentStyle::hoodie: return "hoodie";
    }
    return "jacket";
}

std::optional<TopGarmentStyle> parse_top_garment_style(const std::string_view token) noexcept {
    if (token == "none") return TopGarmentStyle::none;
    if (token == "tunic") return TopGarmentStyle::tunic;
    if (token == "jacket") return TopGarmentStyle::jacket;
    if (token == "trench_coat") return TopGarmentStyle::trench_coat;
    if (token == "armor_cuirass") return TopGarmentStyle::armor_cuirass;
    if (token == "hoodie") return TopGarmentStyle::hoodie;
    return std::nullopt;
}

std::string_view to_token(const BottomGarmentStyle style) noexcept {
    switch (style) {
    case BottomGarmentStyle::none: return "none";
    case BottomGarmentStyle::trousers: return "trousers";
    case BottomGarmentStyle::cargo_pants: return "cargo_pants";
    case BottomGarmentStyle::tactical_greaves: return "tactical_greaves";
    case BottomGarmentStyle::skirt: return "skirt";
    case BottomGarmentStyle::shorts: return "shorts";
    }
    return "cargo_pants";
}

std::optional<BottomGarmentStyle> parse_bottom_garment_style(const std::string_view token) noexcept {
    if (token == "none") return BottomGarmentStyle::none;
    if (token == "trousers") return BottomGarmentStyle::trousers;
    if (token == "cargo_pants") return BottomGarmentStyle::cargo_pants;
    if (token == "tactical_greaves") return BottomGarmentStyle::tactical_greaves;
    if (token == "skirt") return BottomGarmentStyle::skirt;
    if (token == "shorts") return BottomGarmentStyle::shorts;
    return std::nullopt;
}

std::string_view to_token(const FootwearStyle style) noexcept {
    switch (style) {
    case FootwearStyle::barefoot: return "barefoot";
    case FootwearStyle::boots: return "boots";
    case FootwearStyle::combat_boots: return "combat_boots";
    case FootwearStyle::greave_boots: return "greave_boots";
    case FootwearStyle::sneakers: return "sneakers";
    }
    return "combat_boots";
}

std::optional<FootwearStyle> parse_footwear_style(const std::string_view token) noexcept {
    if (token == "barefoot") return FootwearStyle::barefoot;
    if (token == "boots") return FootwearStyle::boots;
    if (token == "combat_boots") return FootwearStyle::combat_boots;
    if (token == "greave_boots") return FootwearStyle::greave_boots;
    if (token == "sneakers") return FootwearStyle::sneakers;
    return std::nullopt;
}

std::string_view to_token(const HairStyle style) noexcept {
    switch (style) {
    case HairStyle::bald: return "bald";
    case HairStyle::short_crop: return "short_crop";
    case HairStyle::spiky: return "spiky";
    case HairStyle::bob: return "bob";
    case HairStyle::ponytail: return "ponytail";
    case HairStyle::mohawk: return "mohawk";
    case HairStyle::hood: return "hood";
    }
    return "short_crop";
}

std::optional<HairStyle> parse_hair_style(const std::string_view token) noexcept {
    if (token == "bald") return HairStyle::bald;
    if (token == "short_crop") return HairStyle::short_crop;
    if (token == "spiky") return HairStyle::spiky;
    if (token == "bob") return HairStyle::bob;
    if (token == "ponytail") return HairStyle::ponytail;
    if (token == "mohawk") return HairStyle::mohawk;
    if (token == "hood") return HairStyle::hood;
    return std::nullopt;
}

std::string_view to_token(const OutfitMaterialPreset preset) noexcept {
    switch (preset) {
    case OutfitMaterialPreset::cloth_linen: return "cloth_linen";
    case OutfitMaterialPreset::leather: return "leather";
    case OutfitMaterialPreset::denim: return "denim";
    case OutfitMaterialPreset::tactical_weave: return "tactical_weave";
    case OutfitMaterialPreset::brushed_steel: return "brushed_steel";
    case OutfitMaterialPreset::gilded_plate: return "gilded_plate";
    }
    return "tactical_weave";
}

std::optional<OutfitMaterialPreset> parse_outfit_material_preset(const std::string_view token) noexcept {
    if (token == "cloth_linen") return OutfitMaterialPreset::cloth_linen;
    if (token == "leather") return OutfitMaterialPreset::leather;
    if (token == "denim") return OutfitMaterialPreset::denim;
    if (token == "tactical_weave") return OutfitMaterialPreset::tactical_weave;
    if (token == "brushed_steel") return OutfitMaterialPreset::brushed_steel;
    if (token == "gilded_plate") return OutfitMaterialPreset::gilded_plate;
    return std::nullopt;
}

std::string_view to_token(const CharacterPaintTarget target) noexcept {
    switch (target) {
    case CharacterPaintTarget::skin: return "skin";
    case CharacterPaintTarget::outfit_primary: return "outfit_primary";
    case CharacterPaintTarget::outfit_secondary: return "outfit_secondary";
    case CharacterPaintTarget::hair: return "hair";
    }
    return "skin";
}

std::optional<CharacterPaintTarget> parse_character_paint_target(const std::string_view token) noexcept {
    if (token == "skin") return CharacterPaintTarget::skin;
    if (token == "outfit_primary") return CharacterPaintTarget::outfit_primary;
    if (token == "outfit_secondary") return CharacterPaintTarget::outfit_secondary;
    if (token == "hair") return CharacterPaintTarget::hair;
    return std::nullopt;
}

std::string_view to_token(const CharacterPaintMode mode) noexcept {
    switch (mode) {
    case CharacterPaintMode::color: return "color";
    case CharacterPaintMode::scar: return "scar";
    case CharacterPaintMode::metallic_accent: return "metallic_accent";
    case CharacterPaintMode::erase: return "erase";
    }
    return "color";
}

std::optional<CharacterPaintMode> parse_character_paint_mode(const std::string_view token) noexcept {
    if (token == "color") return CharacterPaintMode::color;
    if (token == "scar") return CharacterPaintMode::scar;
    if (token == "metallic_accent") return CharacterPaintMode::metallic_accent;
    if (token == "erase") return CharacterPaintMode::erase;
    return std::nullopt;
}

std::string_view to_token(const CharacterTemperament temperament) noexcept {
    switch (temperament) {
    case CharacterTemperament::neutral: return "neutral";
    case CharacterTemperament::warm: return "warm";
    case CharacterTemperament::stern: return "stern";
    case CharacterTemperament::cheerful: return "cheerful";
    case CharacterTemperament::guarded: return "guarded";
    case CharacterTemperament::heroic: return "heroic";
    }
    return "neutral";
}

std::optional<CharacterTemperament> parse_character_temperament(const std::string_view token) noexcept {
    if (token == "neutral") return CharacterTemperament::neutral;
    if (token == "warm") return CharacterTemperament::warm;
    if (token == "stern") return CharacterTemperament::stern;
    if (token == "cheerful") return CharacterTemperament::cheerful;
    if (token == "guarded") return CharacterTemperament::guarded;
    if (token == "heroic") return CharacterTemperament::heroic;
    return std::nullopt;
}

// ============================================================================
// Package Lifecycle, Sanitization & Non-Destructive Serialization
// ============================================================================

std::string sanitize_character_slug(const std::string_view input) {
    std::string out;
    out.reserve(input.size());
    bool last_underscore = true;
    for (const char ch : input) {
        const auto uch = static_cast<unsigned char>(ch);
        if ((uch >= 'a' && uch <= 'z') || (uch >= '0' && uch <= '9')) {
            out.push_back(static_cast<char>(uch));
            last_underscore = false;
        } else if (uch >= 'A' && uch <= 'Z') {
            out.push_back(static_cast<char>(uch - 'A' + 'a'));
            last_underscore = false;
        } else if (!last_underscore && (uch == '_' || uch == '-' || uch == ' ')) {
            out.push_back('_');
            last_underscore = true;
        }
    }
    while (!out.empty() && out.back() == '_') {
        out.pop_back();
    }
    if (out.empty()) {
        return "character";
    }
    if (out.size() > 48U) {
        out.resize(48U);
        while (!out.empty() && out.back() == '_') {
            out.pop_back();
        }
        if (out.empty()) {
            return "character";
        }
    }
    return out;
}

CharacterPackage make_default_character_package(
    const std::string_view slug,
    const std::string_view display_name) {
    CharacterPackage pkg{};
    pkg.identity.slug = sanitize_character_slug(slug);
    pkg.identity.display_name =
        display_name.empty() ? "Unnamed Character" : std::string(display_name);
    pkg.identity.role = CharacterRole::npc;
    pkg.dialogue.dialogue_id = pkg.identity.slug + "_dialogue";
    pkg.dialogue.entry_node_id = "greeting";
    pkg.dialogue.nodes = {
        DialogueNodeSpec{
            .id = "greeting",
            .speaker = pkg.identity.display_name,
            .text = "Hold there, traveler. The perimeter relay went silent an hour ago.",
            .text_ru = "Стой, путник. Реле периметра замолчало час назад.",
            .voice_clip_id = "greeting",
            .expression_clip = "talk",
            .smile = 0.08F,
            .brow_raise = 0.18F,
            .frown = 0.05F,
            .choices = {
                DialogueChoiceSpec{
                    .text = "What happened at the relay?",
                    .text_ru = "Что случилось на ретрансляторе?",
                    .next_node_id = "relay_briefing",
                    .set_var = "asked_relay",
                    .set_value = 1.0F,
                    .closes_dialogue = false,
                },
                DialogueChoiceSpec{
                    .text = "Stay sharp. I will scout ahead.",
                    .text_ru = "Держись начеку. Я пойду в разведку.",
                    .next_node_id = "farewell",
                    .set_var = "scout_accepted",
                    .set_value = 1.0F,
                    .closes_dialogue = false,
                },
            },
        },
        DialogueNodeSpec{
            .id = "relay_briefing",
            .speaker = pkg.identity.display_name,
            .text = "Thermal sensors tripped along the northern ridge before communications cut out.",
            .text_ru = "Термодатчики сработали на северном хребте перед самой потерей связи.",
            .voice_clip_id = "relay_briefing",
            .expression_clip = "talk",
            .smile = 0.0F,
            .brow_raise = 0.12F,
            .frown = 0.22F,
            .choices = {
                DialogueChoiceSpec{
                    .text = "Understood. Moving out now.",
                    .text_ru = "Понял. Выдвигаюсь сейчас же.",
                    .next_node_id = "",
                    .set_var = "mission_active",
                    .set_value = 1.0F,
                    .closes_dialogue = true,
                },
            },
        },
        DialogueNodeSpec{
            .id = "farewell",
            .speaker = pkg.identity.display_name,
            .text = "Good hunting. Signal once you reach the ridge beacon.",
            .text_ru = "Удачной охоты. Подай сигнал, как доберёшься до маяка на хребте.",
            .voice_clip_id = "farewell",
            .expression_clip = "wave",
            .smile = 0.35F,
            .brow_raise = 0.20F,
            .frown = 0.0F,
            .choices = {
                DialogueChoiceSpec{
                    .text = "Leave",
                    .text_ru = "Уйти",
                    .next_node_id = "",
                    .set_var = "",
                    .set_value = 1.0F,
                    .closes_dialogue = true,
                },
            },
        },
    };
    sanitize_character_package(pkg);
    return pkg;
}

void sanitize_character_package(CharacterPackage& p) noexcept {
    p.version = character_package_version;
    p.identity.slug = sanitize_character_slug(p.identity.slug);
    if (p.identity.display_name.empty()) {
        p.identity.display_name = "Character";
    }

    auto& b = p.body;
    if (b.gender != CharacterGender::female && b.gender != CharacterGender::male) {
        b.gender = CharacterGender::male;
    }
    b.height_meters = clamp_finite(b.height_meters, 1.35F, 2.35F, 1.78F);
    b.shoulder_width = clamp_finite(b.shoulder_width, 0.0F, 1.0F, 0.52F);
    b.chest_depth = clamp_finite(b.chest_depth, 0.0F, 1.0F, 0.50F);
    b.waist_width = clamp_finite(b.waist_width, 0.0F, 1.0F, 0.46F);
    b.hip_width = clamp_finite(b.hip_width, 0.0F, 1.0F, 0.48F);
    b.musculature = clamp_finite(b.musculature, 0.0F, 1.0F, 0.48F);
    b.body_fat = clamp_finite(b.body_fat, 0.0F, 1.0F, 0.20F);
    b.arm_length = clamp_finite(b.arm_length, 0.0F, 1.0F, 0.50F);
    b.leg_length = clamp_finite(b.leg_length, 0.0F, 1.0F, 0.50F);
    b.face.head_scale = clamp_finite(b.face.head_scale, 0.72F, 1.38F, 1.0F);
    b.face.jaw_width = clamp_finite(b.face.jaw_width, 0.0F, 1.0F, 0.50F);
    b.face.chin_length = clamp_finite(b.face.chin_length, 0.0F, 1.0F, 0.50F);
    b.face.cheekbone_prominence = clamp_finite(b.face.cheekbone_prominence, 0.0F, 1.0F, 0.50F);
    b.face.brow_ridge = clamp_finite(b.face.brow_ridge, 0.0F, 1.0F, 0.45F);
    b.face.nose_length = clamp_finite(b.face.nose_length, 0.0F, 1.0F, 0.50F);
    b.face.nose_width = clamp_finite(b.face.nose_width, 0.0F, 1.0F, 0.45F);
    b.face.eye_size = clamp_finite(b.face.eye_size, 0.0F, 1.0F, 0.55F);
    b.face.eye_spacing = clamp_finite(b.face.eye_spacing, 0.0F, 1.0F, 0.50F);
    b.face.mouth_width = clamp_finite(b.face.mouth_width, 0.0F, 1.0F, 0.50F);
    b.face.lip_fullness = clamp_finite(b.face.lip_fullness, 0.0F, 1.0F, 0.50F);
    b.face.eye_color = clamp_vec3_color(b.face.eye_color, {0.20F, 0.46F, 0.72F});
    b.skin_color = clamp_vec3_color(b.skin_color, {0.82F, 0.63F, 0.51F});
    b.skin_roughness = clamp_finite(b.skin_roughness, 0.18F, 0.96F, 0.54F);
    b.skin_subsurface_strength = clamp_finite(b.skin_subsurface_strength, 0.0F, 1.0F, 0.65F);
    b.freckles = clamp_finite(b.freckles, 0.0F, 1.0F, 0.08F);
    b.age_wear = clamp_finite(b.age_wear, 0.0F, 1.0F, 0.12F);
    b.lip_tint = clamp_finite(b.lip_tint, 0.0F, 1.0F, 0.35F);

    if (p.sculpt_strokes.size() > max_sculpt_strokes) {
        p.sculpt_strokes.resize(max_sculpt_strokes);
    }
    for (SculptStrokeRecord& s : p.sculpt_strokes) {
        s.radius = clamp_finite(s.radius, 0.015F, 0.65F, 0.08F);
        s.strength = clamp_finite(s.strength, 0.01F, 1.0F, 0.50F);
    }

    auto& w = p.wardrobe;
    w.primary_color = clamp_vec3_color(w.primary_color, {0.18F, 0.29F, 0.42F});
    w.secondary_color = clamp_vec3_color(w.secondary_color, {0.20F, 0.19F, 0.21F});
    w.accent_color = clamp_vec3_color(w.accent_color, {0.82F, 0.64F, 0.28F});
    w.footwear_color = clamp_vec3_color(w.footwear_color, {0.14F, 0.12F, 0.11F});
    w.hair_color = clamp_vec3_color(w.hair_color, {0.16F, 0.11F, 0.08F});
    w.top_thickness = clamp_finite(w.top_thickness, 0.005F, 0.045F, 0.016F);
    w.bottom_thickness = clamp_finite(w.bottom_thickness, 0.005F, 0.040F, 0.014F);
    w.wear_and_tear = clamp_finite(w.wear_and_tear, 0.0F, 1.0F, 0.15F);

    if (p.paint_strokes.size() > max_paint_strokes) {
        p.paint_strokes.resize(max_paint_strokes);
    }
    for (CharacterPaintStroke& pt : p.paint_strokes) {
        pt.uv.x = clamp_finite(pt.uv.x, 0.0F, 1.0F, 0.5F);
        pt.uv.y = clamp_finite(pt.uv.y, 0.0F, 1.0F, 0.5F);
        pt.radius = clamp_finite(pt.radius, 0.004F, 0.45F, 0.045F);
        pt.hardness = clamp_finite(pt.hardness, 0.05F, 1.0F, 0.75F);
        pt.strength = clamp_finite(pt.strength, 0.01F, 1.0F, 0.85F);
        pt.color = clamp_vec4_color(pt.color, {0.88F, 0.22F, 0.18F, 1.0F});
    }

    p.voice.pitch_hz = clamp_finite(p.voice.pitch_hz, 70.0F, 340.0F, 135.0F);
    p.voice.speech_rate = clamp_finite(p.voice.speech_rate, 0.55F, 1.80F, 1.0F);
    p.voice.cadence_variance = clamp_finite(p.voice.cadence_variance, 0.0F, 1.0F, 0.35F);
    p.voice.breathiness = clamp_finite(p.voice.breathiness, 0.0F, 0.65F, 0.12F);
    p.voice.warmth = clamp_finite(p.voice.warmth, 0.0F, 1.0F, 0.55F);

    p.acting.walk_speed_mps = clamp_finite(p.acting.walk_speed_mps, 0.5F, 5.0F, 1.85F);
    p.acting.run_speed_mps = clamp_finite(p.acting.run_speed_mps, 2.0F, 12.0F, 4.90F);
    p.acting.idle_breath_amplitude = clamp_finite(p.acting.idle_breath_amplitude, 0.0F, 2.5F, 1.0F);
    p.acting.stride_energy = clamp_finite(p.acting.stride_energy, 0.2F, 2.0F, 1.0F);
    p.acting.arm_swing = clamp_finite(p.acting.arm_swing, 0.0F, 2.0F, 1.0F);
    p.acting.blink_interval_seconds = clamp_finite(p.acting.blink_interval_seconds, 0.8F, 10.0F, 3.2F);
    p.acting.default_smile = clamp_finite(p.acting.default_smile, 0.0F, 1.0F, 0.10F);
    p.acting.default_brow_raise = clamp_finite(p.acting.default_brow_raise, 0.0F, 1.0F, 0.05F);
    p.acting.default_frown = clamp_finite(p.acting.default_frown, 0.0F, 1.0F, 0.0F);
    if (p.acting.default_idle_clip.empty()) {
        p.acting.default_idle_clip = "idle";
    }

    if (p.dialogue.dialogue_id.empty()) {
        p.dialogue.dialogue_id = p.identity.slug + "_dialogue";
    }
    if (p.dialogue.nodes.size() > max_dialogue_nodes) {
        p.dialogue.nodes.resize(max_dialogue_nodes);
    }
    for (std::size_t i = 0U; i < p.dialogue.nodes.size(); ++i) {
        auto& n = p.dialogue.nodes[i];
        n.id = sanitize_character_slug(n.id.empty() ? ("node_" + std::to_string(i)) : n.id);
        if (n.speaker.empty()) {
            n.speaker = p.identity.display_name;
        }
        if (n.text.empty()) {
            n.text = "...";
        }
        if (n.text_ru.empty()) {
            n.text_ru = n.text;
        }
        if (n.voice_clip_id.empty()) {
            n.voice_clip_id = n.id;
        } else {
            n.voice_clip_id = sanitize_character_slug(n.voice_clip_id);
        }
        if (n.expression_clip.empty()) {
            n.expression_clip = "talk";
        }
        n.smile = clamp_finite(n.smile, 0.0F, 1.0F, 0.1F);
        n.brow_raise = clamp_finite(n.brow_raise, 0.0F, 1.0F, 0.1F);
        n.frown = clamp_finite(n.frown, 0.0F, 1.0F, 0.0F);
        if (n.choices.size() > max_dialogue_choices_per_node) {
            n.choices.resize(max_dialogue_choices_per_node);
        }
        for (DialogueChoiceSpec& c : n.choices) {
            if (c.text.empty()) {
                c.text = "Continue";
            }
            if (c.text_ru.empty()) {
                c.text_ru = c.text;
            }
            c.set_value = clamp_finite(c.set_value, -10000.0F, 10000.0F, 1.0F);
        }
    }
    if (p.dialogue.nodes.empty()) {
        p.dialogue.nodes.push_back(DialogueNodeSpec{
            .id = "greeting",
            .speaker = p.identity.display_name,
            .text = "Greetings.",
            .text_ru = "Приветствую.",
            .voice_clip_id = "greeting",
            .expression_clip = "talk",
        });
    }
    bool entry_exists = false;
    for (const auto& n : p.dialogue.nodes) {
        if (n.id == p.dialogue.entry_node_id) {
            entry_exists = true;
            break;
        }
    }
    if (!entry_exists) {
        p.dialogue.entry_node_id = p.dialogue.nodes.front().id;
    }
}

bool load_character_package_file(
    const std::filesystem::path& path,
    CharacterPackage& out_package,
    std::string& error) {
    error.clear();
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "Unable to open character package '" + utf8_from_path(path) + "'.";
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    Json doc;
    try {
        doc = Json::parse(ss.str());
    } catch (const Json::exception& ex) {
        error = "Invalid JSON in character package '" + utf8_from_path(path) +
                "': " + ex.what();
        return false;
    }
    if (!doc.is_object()) {
        error = "Character package root must be a JSON object.";
        return false;
    }
    if (doc.value("schema", "") != character_package_schema) {
        error = "Unsupported character package schema (expected '" +
                std::string(character_package_schema) + "').";
        return false;
    }
    const std::uint32_t ver = doc.value("version", 0U);
    if (ver != character_package_version) {
        error = "Unsupported character package version: " + std::to_string(ver);
        return false;
    }

    CharacterPackage pkg = make_default_character_package(
        path.stem().stem().string(), "Character");
    pkg.extra_root_json = extract_unknown_keys_json(
        doc,
        {"schema", "version", "identity", "body", "sculpt_strokes",
         "wardrobe", "paint_strokes", "voice", "acting", "dialogue"});

    if (doc.contains("identity") && doc["identity"].is_object()) {
        const Json& id = doc["identity"];
        pkg.identity.slug = id.value("slug", pkg.identity.slug);
        pkg.identity.display_name = id.value("display_name", pkg.identity.display_name);
        pkg.identity.prompt = id.value("prompt", pkg.identity.prompt);
        pkg.identity.seed = id.value("seed", pkg.identity.seed);
        if (const auto r = parse_character_role(id.value("role", "npc"))) {
            pkg.identity.role = *r;
        }
    }

    if (doc.contains("body") && doc["body"].is_object()) {
        const Json& b = doc["body"];
        pkg.extra_body_json = extract_unknown_keys_json(
            b,
            {"gender", "archetype", "height_meters", "shoulder_width", "chest_depth",
             "waist_width", "hip_width", "musculature", "body_fat",
             "arm_length", "leg_length", "face", "skin_color",
             "skin_roughness", "skin_subsurface_strength", "freckles",
             "age_wear", "lip_tint"});
        if (const auto g = parse_character_gender(b.value("gender", "male"))) {
            pkg.body.gender = *g;
        }
        if (const auto a = parse_character_archetype(b.value("archetype", "human_balanced"))) {
            pkg.body.archetype = *a;
        }
        pkg.body.height_meters = b.value("height_meters", pkg.body.height_meters);
        pkg.body.shoulder_width = b.value("shoulder_width", pkg.body.shoulder_width);
        pkg.body.chest_depth = b.value("chest_depth", pkg.body.chest_depth);
        pkg.body.waist_width = b.value("waist_width", pkg.body.waist_width);
        pkg.body.hip_width = b.value("hip_width", pkg.body.hip_width);
        pkg.body.musculature = b.value("musculature", pkg.body.musculature);
        pkg.body.body_fat = b.value("body_fat", pkg.body.body_fat);
        pkg.body.arm_length = b.value("arm_length", pkg.body.arm_length);
        pkg.body.leg_length = b.value("leg_length", pkg.body.leg_length);
        if (b.contains("skin_color")) {
            (void)read_vec3_json(b["skin_color"], pkg.body.skin_color);
        }
        pkg.body.skin_roughness = b.value("skin_roughness", pkg.body.skin_roughness);
        pkg.body.skin_subsurface_strength = b.value("skin_subsurface_strength", pkg.body.skin_subsurface_strength);
        pkg.body.freckles = b.value("freckles", pkg.body.freckles);
        pkg.body.age_wear = b.value("age_wear", pkg.body.age_wear);
        pkg.body.lip_tint = b.value("lip_tint", pkg.body.lip_tint);

        if (b.contains("face") && b["face"].is_object()) {
            const Json& f = b["face"];
            pkg.body.face.head_scale = f.value("head_scale", pkg.body.face.head_scale);
            pkg.body.face.jaw_width = f.value("jaw_width", pkg.body.face.jaw_width);
            pkg.body.face.chin_length = f.value("chin_length", pkg.body.face.chin_length);
            pkg.body.face.cheekbone_prominence = f.value("cheekbone_prominence", pkg.body.face.cheekbone_prominence);
            pkg.body.face.brow_ridge = f.value("brow_ridge", pkg.body.face.brow_ridge);
            pkg.body.face.nose_length = f.value("nose_length", pkg.body.face.nose_length);
            pkg.body.face.nose_width = f.value("nose_width", pkg.body.face.nose_width);
            pkg.body.face.eye_size = f.value("eye_size", pkg.body.face.eye_size);
            pkg.body.face.eye_spacing = f.value("eye_spacing", pkg.body.face.eye_spacing);
            pkg.body.face.mouth_width = f.value("mouth_width", pkg.body.face.mouth_width);
            pkg.body.face.lip_fullness = f.value("lip_fullness", pkg.body.face.lip_fullness);
            if (f.contains("eye_color")) {
                (void)read_vec3_json(f["eye_color"], pkg.body.face.eye_color);
            }
        }
    }

    if (doc.contains("sculpt_strokes") && doc["sculpt_strokes"].is_array()) {
        pkg.sculpt_strokes.clear();
        for (const Json& s : doc["sculpt_strokes"]) {
            if (!s.is_object()) continue;
            SculptStrokeRecord rec{};
            if (const auto k = parse_sculpt_brush_kind(s.value("brush", "inflate"))) {
                rec.brush = *k;
            }
            if (s.contains("local_center")) {
                (void)read_vec3_json(s["local_center"], rec.local_center);
            }
            if (s.contains("displacement")) {
                (void)read_vec3_json(s["displacement"], rec.displacement);
            }
            rec.radius = s.value("radius", rec.radius);
            rec.strength = s.value("strength", rec.strength);
            rec.mirror_x = s.value("mirror_x", rec.mirror_x);
            pkg.sculpt_strokes.push_back(rec);
        }
    }

    if (doc.contains("wardrobe") && doc["wardrobe"].is_object()) {
        const Json& w = doc["wardrobe"];
        pkg.extra_wardrobe_json = extract_unknown_keys_json(
            w,
            {"top_style", "bottom_style", "footwear_style", "hair_style",
             "primary_material", "secondary_material", "has_belt",
             "has_pauldrons", "has_gloves", "has_cape", "has_beard",
             "occlude_covered_body", "studio_underwear", "primary_color",
             "secondary_color", "accent_color", "footwear_color",
             "hair_color", "top_thickness", "bottom_thickness", "wear_and_tear"});
        if (const auto ts = parse_top_garment_style(w.value("top_style", "none"))) {
            pkg.wardrobe.top_style = *ts;
        }
        if (const auto bs = parse_bottom_garment_style(w.value("bottom_style", "none"))) {
            pkg.wardrobe.bottom_style = *bs;
        }
        if (const auto fs = parse_footwear_style(w.value("footwear_style", "barefoot"))) {
            pkg.wardrobe.footwear_style = *fs;
        }
        if (const auto hs = parse_hair_style(w.value("hair_style", "short_crop"))) {
            pkg.wardrobe.hair_style = *hs;
        }
        if (const auto pm = parse_outfit_material_preset(w.value("primary_material", "tactical_weave"))) {
            pkg.wardrobe.primary_material = *pm;
        }
        if (const auto sm = parse_outfit_material_preset(w.value("secondary_material", "leather"))) {
            pkg.wardrobe.secondary_material = *sm;
        }
        pkg.wardrobe.has_belt = w.value("has_belt", pkg.wardrobe.has_belt);
        pkg.wardrobe.has_pauldrons = w.value("has_pauldrons", pkg.wardrobe.has_pauldrons);
        pkg.wardrobe.has_gloves = w.value("has_gloves", pkg.wardrobe.has_gloves);
        pkg.wardrobe.has_cape = w.value("has_cape", pkg.wardrobe.has_cape);
        pkg.wardrobe.has_beard = w.value("has_beard", pkg.wardrobe.has_beard);
        pkg.wardrobe.occlude_covered_body = w.value("occlude_covered_body", pkg.wardrobe.occlude_covered_body);
        pkg.wardrobe.studio_underwear = w.value("studio_underwear", pkg.wardrobe.studio_underwear);
        if (w.contains("primary_color")) (void)read_vec3_json(w["primary_color"], pkg.wardrobe.primary_color);
        if (w.contains("secondary_color")) (void)read_vec3_json(w["secondary_color"], pkg.wardrobe.secondary_color);
        if (w.contains("accent_color")) (void)read_vec3_json(w["accent_color"], pkg.wardrobe.accent_color);
        if (w.contains("footwear_color")) (void)read_vec3_json(w["footwear_color"], pkg.wardrobe.footwear_color);
        if (w.contains("hair_color")) (void)read_vec3_json(w["hair_color"], pkg.wardrobe.hair_color);
        pkg.wardrobe.top_thickness = w.value("top_thickness", pkg.wardrobe.top_thickness);
        pkg.wardrobe.bottom_thickness = w.value("bottom_thickness", pkg.wardrobe.bottom_thickness);
        pkg.wardrobe.wear_and_tear = w.value("wear_and_tear", pkg.wardrobe.wear_and_tear);
    }

    if (doc.contains("paint_strokes") && doc["paint_strokes"].is_array()) {
        pkg.paint_strokes.clear();
        for (const Json& pt : doc["paint_strokes"]) {
            if (!pt.is_object()) continue;
            CharacterPaintStroke st{};
            if (const auto t = parse_character_paint_target(pt.value("target", "outfit_primary"))) {
                st.target = *t;
            }
            if (const auto m = parse_character_paint_mode(pt.value("mode", "color"))) {
                st.mode = *m;
            }
            if (pt.contains("uv")) (void)read_vec2_json(pt["uv"], st.uv);
            st.radius = pt.value("radius", st.radius);
            st.hardness = pt.value("hardness", st.hardness);
            st.strength = pt.value("strength", st.strength);
            if (pt.contains("color")) (void)read_vec4_json(pt["color"], st.color);
            st.mirror_u = pt.value("mirror_u", st.mirror_u);
            pkg.paint_strokes.push_back(st);
        }
    }

    if (doc.contains("voice") && doc["voice"].is_object()) {
        const Json& v = doc["voice"];
        pkg.voice.pitch_hz = v.value("pitch_hz", pkg.voice.pitch_hz);
        pkg.voice.speech_rate = v.value("speech_rate", pkg.voice.speech_rate);
        pkg.voice.cadence_variance = v.value("cadence_variance", pkg.voice.cadence_variance);
        pkg.voice.breathiness = v.value("breathiness", pkg.voice.breathiness);
        pkg.voice.warmth = v.value("warmth", pkg.voice.warmth);
    }

    if (doc.contains("acting") && doc["acting"].is_object()) {
        const Json& a = doc["acting"];
        pkg.extra_acting_json = extract_unknown_keys_json(
            a,
            {"walk_speed_mps", "run_speed_mps", "idle_breath_amplitude",
             "stride_energy", "arm_swing", "blink_interval_seconds",
             "temperament", "default_idle_clip", "default_smile",
             "default_brow_raise", "default_frown"});
        pkg.acting.walk_speed_mps = a.value("walk_speed_mps", pkg.acting.walk_speed_mps);
        pkg.acting.run_speed_mps = a.value("run_speed_mps", pkg.acting.run_speed_mps);
        pkg.acting.idle_breath_amplitude = a.value("idle_breath_amplitude", pkg.acting.idle_breath_amplitude);
        pkg.acting.stride_energy = a.value("stride_energy", pkg.acting.stride_energy);
        pkg.acting.arm_swing = a.value("arm_swing", pkg.acting.arm_swing);
        pkg.acting.blink_interval_seconds = a.value("blink_interval_seconds", pkg.acting.blink_interval_seconds);
        if (const auto t = parse_character_temperament(a.value("temperament", "neutral"))) {
            pkg.acting.temperament = *t;
        }
        pkg.acting.default_idle_clip = a.value("default_idle_clip", pkg.acting.default_idle_clip);
        pkg.acting.default_smile = a.value("default_smile", pkg.acting.default_smile);
        pkg.acting.default_brow_raise = a.value("default_brow_raise", pkg.acting.default_brow_raise);
        pkg.acting.default_frown = a.value("default_frown", pkg.acting.default_frown);
    }

    if (doc.contains("dialogue") && doc["dialogue"].is_object()) {
        const Json& d = doc["dialogue"];
        pkg.extra_dialogue_json = extract_unknown_keys_json(
            d, {"dialogue_id", "entry_node_id", "nodes"});
        pkg.dialogue.dialogue_id = d.value("dialogue_id", pkg.dialogue.dialogue_id);
        pkg.dialogue.entry_node_id = d.value("entry_node_id", pkg.dialogue.entry_node_id);
        if (d.contains("nodes") && d["nodes"].is_array()) {
            pkg.dialogue.nodes.clear();
            for (const Json& n : d["nodes"]) {
                if (!n.is_object()) continue;
                DialogueNodeSpec node{};
                node.id = n.value("id", "");
                node.speaker = n.value("speaker", pkg.identity.display_name);
                node.text = n.value("text", "");
                node.text_ru = n.value("text_ru", "");
                node.voice_clip_id = n.value("voice_clip_id", node.id);
                node.expression_clip = n.value("expression_clip", "talk");
                node.smile = n.value("smile", 0.1F);
                node.brow_raise = n.value("brow_raise", 0.1F);
                node.frown = n.value("frown", 0.0F);
                if (n.contains("choices") && n["choices"].is_array()) {
                    for (const Json& c : n["choices"]) {
                        if (!c.is_object()) continue;
                        DialogueChoiceSpec ch{};
                        ch.text = c.value("text", "");
                        ch.text_ru = c.value("text_ru", "");
                        ch.next_node_id = c.value("next_node_id", "");
                        ch.set_var = c.value("set_var", "");
                        ch.set_value = c.value("set_value", 1.0F);
                        ch.closes_dialogue = c.value("closes_dialogue", false);
                        node.choices.push_back(std::move(ch));
                    }
                }
                pkg.dialogue.nodes.push_back(std::move(node));
            }
        }
    }

    sanitize_character_package(pkg);
    out_package = std::move(pkg);
    return true;
}

bool save_character_package_file(
    const std::filesystem::path& path,
    const CharacterPackage& input_package,
    std::string& error) {
    CharacterPackage p = input_package;
    sanitize_character_package(p);

    Json body_json{
        {"gender", to_token(p.body.gender)},
        {"archetype", to_token(p.body.archetype)},
        {"height_meters", p.body.height_meters},
        {"shoulder_width", p.body.shoulder_width},
        {"chest_depth", p.body.chest_depth},
        {"waist_width", p.body.waist_width},
        {"hip_width", p.body.hip_width},
        {"musculature", p.body.musculature},
        {"body_fat", p.body.body_fat},
        {"arm_length", p.body.arm_length},
        {"leg_length", p.body.leg_length},
        {"skin_color", vec3_to_json(p.body.skin_color)},
        {"skin_roughness", p.body.skin_roughness},
        {"skin_subsurface_strength", p.body.skin_subsurface_strength},
        {"freckles", p.body.freckles},
        {"age_wear", p.body.age_wear},
        {"lip_tint", p.body.lip_tint},
        {"face",
         {
             {"head_scale", p.body.face.head_scale},
             {"jaw_width", p.body.face.jaw_width},
             {"chin_length", p.body.face.chin_length},
             {"cheekbone_prominence", p.body.face.cheekbone_prominence},
             {"brow_ridge", p.body.face.brow_ridge},
             {"nose_length", p.body.face.nose_length},
             {"nose_width", p.body.face.nose_width},
             {"eye_size", p.body.face.eye_size},
             {"eye_spacing", p.body.face.eye_spacing},
             {"mouth_width", p.body.face.mouth_width},
             {"lip_fullness", p.body.face.lip_fullness},
             {"eye_color", vec3_to_json(p.body.face.eye_color)},
         }},
    };
    merge_extra_object(body_json, p.extra_body_json);

    Json sculpt_json = Json::array();
    for (const SculptStrokeRecord& s : p.sculpt_strokes) {
        sculpt_json.push_back(Json{
            {"brush", to_token(s.brush)},
            {"local_center", vec3_to_json(s.local_center)},
            {"displacement", vec3_to_json(s.displacement)},
            {"radius", s.radius},
            {"strength", s.strength},
            {"mirror_x", s.mirror_x},
        });
    }

    Json wardrobe_json{
        {"top_style", to_token(p.wardrobe.top_style)},
        {"bottom_style", to_token(p.wardrobe.bottom_style)},
        {"footwear_style", to_token(p.wardrobe.footwear_style)},
        {"hair_style", to_token(p.wardrobe.hair_style)},
        {"primary_material", to_token(p.wardrobe.primary_material)},
        {"secondary_material", to_token(p.wardrobe.secondary_material)},
        {"has_belt", p.wardrobe.has_belt},
        {"has_pauldrons", p.wardrobe.has_pauldrons},
        {"has_gloves", p.wardrobe.has_gloves},
        {"has_cape", p.wardrobe.has_cape},
        {"has_beard", p.wardrobe.has_beard},
        {"occlude_covered_body", p.wardrobe.occlude_covered_body},
        {"studio_underwear", p.wardrobe.studio_underwear},
        {"primary_color", vec3_to_json(p.wardrobe.primary_color)},
        {"secondary_color", vec3_to_json(p.wardrobe.secondary_color)},
        {"accent_color", vec3_to_json(p.wardrobe.accent_color)},
        {"footwear_color", vec3_to_json(p.wardrobe.footwear_color)},
        {"hair_color", vec3_to_json(p.wardrobe.hair_color)},
        {"top_thickness", p.wardrobe.top_thickness},
        {"bottom_thickness", p.wardrobe.bottom_thickness},
        {"wear_and_tear", p.wardrobe.wear_and_tear},
    };
    merge_extra_object(wardrobe_json, p.extra_wardrobe_json);

    Json paint_json = Json::array();
    for (const CharacterPaintStroke& pt : p.paint_strokes) {
        paint_json.push_back(Json{
            {"target", to_token(pt.target)},
            {"mode", to_token(pt.mode)},
            {"uv", vec2_to_json(pt.uv)},
            {"radius", pt.radius},
            {"hardness", pt.hardness},
            {"strength", pt.strength},
            {"color", vec4_to_json(pt.color)},
            {"mirror_u", pt.mirror_u},
        });
    }

    Json acting_json{
        {"walk_speed_mps", p.acting.walk_speed_mps},
        {"run_speed_mps", p.acting.run_speed_mps},
        {"idle_breath_amplitude", p.acting.idle_breath_amplitude},
        {"stride_energy", p.acting.stride_energy},
        {"arm_swing", p.acting.arm_swing},
        {"blink_interval_seconds", p.acting.blink_interval_seconds},
        {"temperament", to_token(p.acting.temperament)},
        {"default_idle_clip", p.acting.default_idle_clip},
        {"default_smile", p.acting.default_smile},
        {"default_brow_raise", p.acting.default_brow_raise},
        {"default_frown", p.acting.default_frown},
    };
    merge_extra_object(acting_json, p.extra_acting_json);

    Json nodes_json = Json::array();
    for (const DialogueNodeSpec& n : p.dialogue.nodes) {
        Json choices_json = Json::array();
        for (const DialogueChoiceSpec& c : n.choices) {
            choices_json.push_back(Json{
                {"text", c.text},
                {"text_ru", c.text_ru},
                {"next_node_id", c.next_node_id},
                {"set_var", c.set_var},
                {"set_value", c.set_value},
                {"closes_dialogue", c.closes_dialogue},
            });
        }
        nodes_json.push_back(Json{
            {"id", n.id},
            {"speaker", n.speaker},
            {"text", n.text},
            {"text_ru", n.text_ru},
            {"voice_clip_id", n.voice_clip_id},
            {"expression_clip", n.expression_clip},
            {"smile", n.smile},
            {"brow_raise", n.brow_raise},
            {"frown", n.frown},
            {"choices", std::move(choices_json)},
        });
    }
    Json dialogue_json{
        {"dialogue_id", p.dialogue.dialogue_id},
        {"entry_node_id", p.dialogue.entry_node_id},
        {"nodes", std::move(nodes_json)},
    };
    merge_extra_object(dialogue_json, p.extra_dialogue_json);

    Json root{
        {"schema", character_package_schema},
        {"version", p.version},
        {"identity",
         {
             {"slug", p.identity.slug},
             {"display_name", p.identity.display_name},
             {"role", to_token(p.identity.role)},
             {"prompt", p.identity.prompt},
             {"seed", p.identity.seed},
         }},
        {"body", std::move(body_json)},
        {"sculpt_strokes", std::move(sculpt_json)},
        {"wardrobe", std::move(wardrobe_json)},
        {"paint_strokes", std::move(paint_json)},
        {"voice",
         {
             {"pitch_hz", p.voice.pitch_hz},
             {"speech_rate", p.voice.speech_rate},
             {"cadence_variance", p.voice.cadence_variance},
             {"breathiness", p.voice.breathiness},
             {"warmth", p.voice.warmth},
         }},
        {"acting", std::move(acting_json)},
        {"dialogue", std::move(dialogue_json)},
    };
    merge_extra_object(root, p.extra_root_json);

    return write_text_file_atomic(path, root.dump(2) + "\n", error);
}

bool load_or_create_character_package(
    const std::filesystem::path& path,
    const std::string_view fallback_slug,
    CharacterPackage& out_package,
    std::string& error) {
    std::error_code ec;
    if (std::filesystem::is_regular_file(path, ec) && !ec) {
        return load_character_package_file(path, out_package, error);
    }
    out_package = make_default_character_package(
        fallback_slug.empty() ? "warden_kane" : fallback_slug,
        "Warden Kane");
    return save_character_package_file(path, out_package, error);
}

std::filesystem::path default_character_package_path(
    const std::filesystem::path& project_directory,
    const std::string_view slug) {
    return project_directory / "characters" /
           (sanitize_character_slug(slug) + ".vcrchar.json");
}

std::vector<std::filesystem::path> discover_project_character_packages(
    const std::filesystem::path& project_directory) {
    std::vector<std::filesystem::path> packages{};
    if (project_directory.empty()) {
        return packages;
    }
    const std::filesystem::path chars_dir = project_directory / "characters";
    std::error_code ec;
    if (!std::filesystem::is_directory(chars_dir, ec) || ec) {
        return packages;
    }
    for (const auto& entry : std::filesystem::directory_iterator(chars_dir, ec)) {
        if (ec) {
            break;
        }
        std::error_code file_ec;
        if (!entry.is_regular_file(file_ec) || file_ec) {
            continue;
        }
        const std::string fname = entry.path().filename().string();
        if (fname.ends_with(".vcrchar.json")) {
            packages.push_back(entry.path());
        }
    }
    std::sort(
        packages.begin(),
        packages.end(),
        [](const std::filesystem::path& a, const std::filesystem::path& b) {
            return a.filename().generic_string() < b.filename().generic_string();
        });
    return packages;
}

// ============================================================================
// Skeleton & Mesh Generation
// ============================================================================

CookedSkeletonAsset build_character_skeleton(const CharacterPackage& input_pkg) {
    CharacterPackage pkg = input_pkg;
    sanitize_character_package(pkg);
    const auto layout = compute_character_bone_layout(pkg.body);

    CookedSkeletonAsset skel{};
    skel.root_motion_bone_index = 0;
    skel.bones.resize(layout.size());
    std::vector<glm::mat4> model_mats(layout.size(), glm::mat4(1.0F));
    for (std::size_t i = 0U; i < layout.size(); ++i) {
        SkeletonBone& b = skel.bones[i];
        b.name = layout[i].name;
        b.parent_index = layout[i].parent;
        b.local_bind_rotation = glm::quat(1.0F, 0.0F, 0.0F, 0.0F);
        b.local_bind_scale = glm::vec3(1.0F);
        if (b.parent_index >= 0 &&
            static_cast<std::size_t>(b.parent_index) < i) {
            b.local_bind_translation =
                layout[i].model_pos - layout[static_cast<std::size_t>(b.parent_index)].model_pos;
        } else {
            b.local_bind_translation = layout[i].model_pos;
        }
        model_mats[i] = glm::translate(glm::mat4(1.0F), layout[i].model_pos);
        b.inverse_bind_matrix = glm::inverse(model_mats[i]);
    }
    return skel;
}

template <typename TriPred, typename OffsetFn, typename UvFn, typename SkinFn>
void emit_template_part_subset(
    MeshBuilder& mb,
    const TemplatePart& part,
    const std::span<const glm::vec3> positions,
    const std::span<const glm::vec3> normals,
    const float rim_depth,
    const CharacterPaintTarget target,
    const TriPred& tri_pred,
    const OffsetFn& offset_fn,
    const UvFn& uv_fn,
    const SkinFn& skin_fn) {
    if (part.corners.empty() || part.indices.size() < 3U) {
        return;
    }

    std::vector<std::uint32_t> kept_corner_indices{};
    kept_corner_indices.reserve(part.indices.size());
    for (std::size_t i = 0U; i + 2U < part.indices.size(); i += 3U) {
        const std::uint32_t i0 = part.indices[i + 0U];
        const std::uint32_t i1 = part.indices[i + 1U];
        const std::uint32_t i2 = part.indices[i + 2U];
        if (i0 >= part.corners.size() || i1 >= part.corners.size() || i2 >= part.corners.size()) {
            continue;
        }
        if (tri_pred(part.corners[i0], part.corners[i1], part.corners[i2])) {
            kept_corner_indices.push_back(i0);
            kept_corner_indices.push_back(i1);
            kept_corner_indices.push_back(i2);
        }
    }
    if (kept_corner_indices.empty()) {
        return;
    }

    std::vector<std::uint32_t> corner_to_mesh(part.corners.size(), (std::numeric_limits<std::uint32_t>::max)());
    for (const std::uint32_t ic : kept_corner_indices) {
        if (corner_to_mesh[ic] == (std::numeric_limits<std::uint32_t>::max)()) {
            const TemplateCorner& c = part.corners[ic];
            const glm::vec3 base_pos = positions[c.vid];
            const glm::vec3 nrm = normals[c.vid];
            const glm::vec3 displaced = offset_fn(c, base_pos, nrm);
            const glm::vec2 uv = uv_fn(c, displaced);
            const CookedSkinVertex sk = skin_fn(c, displaced);
            std::uint32_t out_v = 0U;
            mb.add_triangle_vertex(displaced, nrm, uv, sk, target, out_v);
            corner_to_mesh[ic] = out_v;
        }
    }
    for (std::size_t i = 0U; i + 2U < kept_corner_indices.size(); i += 3U) {
        mb.indices.push_back(corner_to_mesh[kept_corner_indices[i + 0U]]);
        mb.indices.push_back(corner_to_mesh[kept_corner_indices[i + 1U]]);
        mb.indices.push_back(corner_to_mesh[kept_corner_indices[i + 2U]]);
    }

    if (rim_depth <= 1.0e-5F) {
        return;
    }

    std::vector<std::uint64_t> half_edges{};
    half_edges.reserve(kept_corner_indices.size());
    const auto pack_edge = [](const std::uint16_t a, const std::uint16_t b) noexcept -> std::uint64_t {
        return (static_cast<std::uint64_t>(a) << 32U) | static_cast<std::uint64_t>(b);
    };
    for (std::size_t i = 0U; i + 2U < kept_corner_indices.size(); i += 3U) {
        const std::uint16_t v0 = part.corners[kept_corner_indices[i + 0U]].vid;
        const std::uint16_t v1 = part.corners[kept_corner_indices[i + 1U]].vid;
        const std::uint16_t v2 = part.corners[kept_corner_indices[i + 2U]].vid;
        half_edges.push_back(pack_edge(v0, v1));
        half_edges.push_back(pack_edge(v1, v2));
        half_edges.push_back(pack_edge(v2, v0));
    }
    std::sort(half_edges.begin(), half_edges.end());

    const auto emit_rim_quad = [&](const std::uint32_t ica, const std::uint32_t icb) {
        const TemplateCorner& ca = part.corners[ica];
        const TemplateCorner& cb = part.corners[icb];
        if (ca.vid == cb.vid) {
            return;
        }
        const std::uint32_t ma = corner_to_mesh[ica];
        const std::uint32_t mb_idx = corner_to_mesh[icb];
        const CookedModelVertex& va_src = mb.vertices[ma];
        const CookedModelVertex& vb_src = mb.vertices[mb_idx];
        const glm::vec3 pa(va_src.position[0], va_src.position[1], va_src.position[2]);
        const glm::vec3 pb(vb_src.position[0], vb_src.position[1], vb_src.position[2]);
        const glm::vec3 na = normals[ca.vid];
        const glm::vec3 nb = normals[cb.vid];
        const glm::vec3 pa_in = pa - na * rim_depth;
        const glm::vec3 pb_in = pb - nb * rim_depth;
        const glm::vec3 raw_wn = glm::cross(pa_in - pa, pb_in - pa);
        const float wlen = glm::length(raw_wn);
        const glm::vec3 wn = wlen > 1.0e-7F ? (raw_wn / wlen) : -na;
        const glm::vec2 uva(va_src.texcoord[0], 1.0F - va_src.texcoord[1]);
        const glm::vec2 uvb(vb_src.texcoord[0], 1.0F - vb_src.texcoord[1]);
        std::uint32_t r0 = 0U;
        std::uint32_t r1 = 0U;
        std::uint32_t r2 = 0U;
        std::uint32_t r3 = 0U;
        mb.add_triangle_vertex(pa, wn, uva, mb.skin_vertices[ma], target, r0);
        mb.add_triangle_vertex(pa_in, wn, uva, mb.skin_vertices[ma], target, r1);
        mb.add_triangle_vertex(pb_in, wn, uvb, mb.skin_vertices[mb_idx], target, r2);
        mb.add_triangle_vertex(pb, wn, uvb, mb.skin_vertices[mb_idx], target, r3);
        mb.indices.push_back(r0);
        mb.indices.push_back(r1);
        mb.indices.push_back(r2);
        mb.indices.push_back(r0);
        mb.indices.push_back(r2);
        mb.indices.push_back(r3);
    };

    for (std::size_t i = 0U; i + 2U < kept_corner_indices.size(); i += 3U) {
        const std::array<std::uint32_t, 3U> ics{{
            kept_corner_indices[i + 0U],
            kept_corner_indices[i + 1U],
            kept_corner_indices[i + 2U],
        }};
        for (std::size_t e = 0U; e < 3U; ++e) {
            const std::uint32_t ica = ics[e];
            const std::uint32_t icb = ics[(e + 1U) % 3U];
            const std::uint64_t rev = pack_edge(part.corners[icb].vid, part.corners[ica].vid);
            if (!std::binary_search(half_edges.begin(), half_edges.end(), rev)) {
                emit_rim_quad(ica, icb);
            }
        }
    }
}

[[nodiscard]] bool is_region_covered_by_wardrobe(
    const CharacterWardrobeSpec& w,
    const TemplateRegion reg) noexcept {
    const bool has_top = (w.top_style != TopGarmentStyle::none);
    const bool long_sleeves =
        w.top_style == TopGarmentStyle::jacket ||
        w.top_style == TopGarmentStyle::trench_coat ||
        w.top_style == TopGarmentStyle::hoodie;
    const bool long_top_pelvis =
        w.top_style == TopGarmentStyle::trench_coat ||
        w.top_style == TopGarmentStyle::tunic;
    const bool has_bottom = (w.bottom_style != BottomGarmentStyle::none);
    const bool full_legs =
        w.bottom_style == BottomGarmentStyle::trousers ||
        w.bottom_style == BottomGarmentStyle::cargo_pants ||
        w.bottom_style == BottomGarmentStyle::tactical_greaves;
    const bool tall_boots =
        w.footwear_style == FootwearStyle::combat_boots ||
        w.footwear_style == FootwearStyle::greave_boots;

    switch (reg) {
    case TemplateRegion::torso_upper:
        return has_top;
    case TemplateRegion::torso_lower:
        return has_top &&
            w.bottom_style != BottomGarmentStyle::skirt &&
            (has_bottom || w.top_style == TopGarmentStyle::trench_coat);
    case TemplateRegion::pelvis:
        return has_bottom || long_top_pelvis;
    case TemplateRegion::upper_arm:
        return long_sleeves;
    case TemplateRegion::forearm:
        return long_sleeves;
    case TemplateRegion::hand:
        return w.has_gloves;
    case TemplateRegion::thigh_upper:
        return has_bottom && w.bottom_style != BottomGarmentStyle::skirt;
    case TemplateRegion::thigh_lower:
        return full_legs;
    case TemplateRegion::shin:
        return full_legs || tall_boots;
    case TemplateRegion::ankle_boot:
    case TemplateRegion::foot:
        return w.footwear_style != FootwearStyle::barefoot;
    case TemplateRegion::none:
    case TemplateRegion::head:
    case TemplateRegion::neck:
    case TemplateRegion::inner_mouth:
        return false;
    }
    return false;
}

namespace {

void recompute_vertex_normals_range(
    MeshBuilder& mb,
    const std::uint32_t v_start,
    const std::uint32_t i_start) {
    if (mb.vertices.size() <= v_start || i_start >= mb.indices.size() ||
        (mb.indices.size() - i_start) < 3U) {
        return;
    }
    const std::size_t count = mb.vertices.size() - v_start;
    std::vector<glm::vec3> naccum(count, glm::vec3(0.0F));
    for (std::size_t ii = i_start; ii + 2U < mb.indices.size(); ii += 3U) {
        const std::uint32_t ia = mb.indices[ii + 0U];
        const std::uint32_t ib = mb.indices[ii + 1U];
        const std::uint32_t ic = mb.indices[ii + 2U];
        if (ia < v_start || ib < v_start || ic < v_start ||
            ia >= mb.vertices.size() || ib >= mb.vertices.size() || ic >= mb.vertices.size()) {
            continue;
        }
        const glm::vec3 pa(mb.vertices[ia].position[0], mb.vertices[ia].position[1], mb.vertices[ia].position[2]);
        const glm::vec3 pb(mb.vertices[ib].position[0], mb.vertices[ib].position[1], mb.vertices[ib].position[2]);
        const glm::vec3 pc(mb.vertices[ic].position[0], mb.vertices[ic].position[1], mb.vertices[ic].position[2]);
        const glm::vec3 fn = glm::cross(pb - pa, pc - pa);
        naccum[ia - v_start] += fn;
        naccum[ib - v_start] += fn;
        naccum[ic - v_start] += fn;
    }

    struct QuantizedVertNormal {
        std::int32_t qx{0};
        std::int32_t qy{0};
        std::int32_t qz{0};
        std::uint32_t k{0U};
    };

    std::vector<QuantizedVertNormal> sorted;
    sorted.reserve(count);
    std::vector<glm::vec3> unit_norms(count, glm::vec3(0.0F, 1.0F, 0.0F));
    for (std::size_t k = 0U; k < count; ++k) {
        const float len = glm::length(naccum[k]);
        if (len > 1.0e-7F) {
            unit_norms[k] = naccum[k] / len;
        } else {
            const glm::vec3 orig(
                mb.vertices[v_start + k].normal[0],
                mb.vertices[v_start + k].normal[1],
                mb.vertices[v_start + k].normal[2]);
            const float olen = glm::length(orig);
            if (olen > 1.0e-7F) {
                unit_norms[k] = orig / olen;
            }
        }
        const auto& pos = mb.vertices[v_start + k].position;
        sorted.push_back(QuantizedVertNormal{
            static_cast<std::int32_t>(std::lround(pos[0] * 10000.0F)),
            static_cast<std::int32_t>(std::lround(pos[1] * 10000.0F)),
            static_cast<std::int32_t>(std::lround(pos[2] * 10000.0F)),
            static_cast<std::uint32_t>(k),
        });
    }

    std::sort(sorted.begin(), sorted.end(), [](const QuantizedVertNormal& a, const QuantizedVertNormal& b) {
        if (a.qx != b.qx) {
            return a.qx < b.qx;
        }
        if (a.qy != b.qy) {
            return a.qy < b.qy;
        }
        if (a.qz != b.qz) {
            return a.qz < b.qz;
        }
        return a.k < b.k;
    });

    std::vector<glm::vec3> welded(naccum);
    for (std::size_t begin = 0U; begin < sorted.size();) {
        std::size_t end = begin + 1U;
        while (end < sorted.size() &&
               sorted[end].qx == sorted[begin].qx &&
               sorted[end].qy == sorted[begin].qy &&
               sorted[end].qz == sorted[begin].qz) {
            ++end;
        }
        if (end - begin > 1U) {
            for (std::size_t i = begin; i < end; ++i) {
                const std::uint32_t ka = sorted[i].k;
                if (glm::length(naccum[ka]) <= 1.0e-7F) {
                    continue;
                }
                glm::vec3 sum = naccum[ka];
                for (std::size_t j = begin; j < end; ++j) {
                    if (i == j) {
                        continue;
                    }
                    const std::uint32_t kb = sorted[j].k;
                    if (glm::length(naccum[kb]) > 1.0e-7F &&
                        glm::dot(unit_norms[ka], unit_norms[kb]) > 0.35F) {
                        sum += naccum[kb];
                    }
                }
                welded[ka] = sum;
            }
        }
        begin = end;
    }

    for (std::size_t k = 0U; k < count; ++k) {
        const float len = glm::length(welded[k]);
        if (len > 1.0e-7F) {
            const glm::vec3 nn = welded[k] / len;
            mb.vertices[v_start + k].normal = {nn.x, nn.y, nn.z};
        }
    }
}

[[maybe_unused]] [[nodiscard]] glm::vec2 azimuthal_crown_uv(
    const glm::vec3& p,
    const glm::vec3& head_center) noexcept {
    const glm::vec3 d = p - head_center;
    const float len = glm::length(d);
    const glm::vec3 dir = len > 1.0e-5F ? (d / len) : glm::vec3(0.0F, 1.0F, 0.0F);
    const float polar = std::acos(std::clamp(dir.y, -1.0F, 1.0F)) / glm::pi<float>();
    const float lxz = std::hypot(dir.x, dir.z);
    const float ux = lxz > 1.0e-5F ? dir.x / lxz : 0.0F;
    const float uz = lxz > 1.0e-5F ? dir.z / lxz : 1.0F;
    return glm::vec2(
        std::clamp(0.5F + ux * polar * 0.60F, 0.04F, 0.96F),
        std::clamp(0.5F + uz * polar * 0.60F, 0.04F, 0.96F));
}

struct ClipVert {
    glm::vec3 base_p{0.0F};
    glm::vec3 nrm{0.0F, 1.0F, 0.0F};
    float orig_u{0.0F};
    float orig_v{0.0F};
    CookedSkinVertex skin{};
    std::uint8_t region{0U};
    std::uint8_t flags{0U};
    std::uint8_t cut_mask{0U};
};

[[nodiscard]] CookedSkinVertex lerp_skin_vertex(
    const CookedSkinVertex& a,
    const CookedSkinVertex& b,
    const float t) noexcept {
    struct Entry {
        std::uint16_t joint{0U};
        float weight{0.0F};
    };
    std::array<Entry, 8U> entries{};
    std::size_t n = 0U;
    const auto accumulate = [&](const CookedSkinVertex& sv, const float factor) noexcept {
        if (factor <= 1.0e-6F) {
            return;
        }
        for (std::size_t k = 0U; k < 4U; ++k) {
            const float w = sv.weights[k] * factor;
            const std::uint16_t j = sv.joints[k];
            if (w <= 1.0e-6F || j >= character_total_bone_count) {
                continue;
            }
            bool merged = false;
            for (std::size_t m = 0U; m < n; ++m) {
                if (entries[m].joint == j) {
                    entries[m].weight += w;
                    merged = true;
                    break;
                }
            }
            if (!merged && n < entries.size()) {
                entries[n++] = Entry{j, w};
            }
        }
    };
    accumulate(a, 1.0F - t);
    accumulate(b, t);

    if (n == 0U) {
        CookedSkinVertex fallback{};
        fallback.joints = {0U, 0U, 0U, 0U};
        fallback.weights = {1.0F, 0.0F, 0.0F, 0.0F};
        return fallback;
    }

    std::sort(
        entries.begin(),
        entries.begin() + static_cast<std::ptrdiff_t>(n),
        [](const Entry& lhs, const Entry& rhs) noexcept {
            return lhs.weight > rhs.weight;
        });

    const std::size_t keep = (std::min)(n, std::size_t{4U});
    float sum = 0.0F;
    for (std::size_t k = 0U; k < keep; ++k) {
        sum += entries[k].weight;
    }
    CookedSkinVertex out{};
    if (sum <= 1.0e-6F) {
        out.joints = {0U, 0U, 0U, 0U};
        out.weights = {1.0F, 0.0F, 0.0F, 0.0F};
        return out;
    }
    const float inv = 1.0F / sum;
    for (std::size_t k = 0U; k < keep; ++k) {
        out.joints[k] = entries[k].joint;
        out.weights[k] = entries[k].weight * inv;
    }
    return out;
}

template <typename TriAllowFn, typename PlaneEvalFn, typename OffsetFn, typename UvFn>
void emit_clipped_template_part(
    MeshBuilder& mb,
    const TemplatePart& part,
    const std::span<const glm::vec3> positions,
    const std::span<const glm::vec3> normals,
    const std::span<const CookedSkinVertex> skin_weights,
    const float rim_depth,
    const CharacterPaintTarget target,
    const TriAllowFn& tri_allow_fn,
    const std::size_t plane_count,
    const PlaneEvalFn& plane_eval_fn,
    const OffsetFn& offset_fn,
    const UvFn& uv_fn) {
    if (part.corners.empty() || part.indices.size() < 3U) {
        return;
    }

    const auto make_clip_vert = [&](const TemplateCorner& c) noexcept -> ClipVert {
        ClipVert cv{};
        cv.base_p = positions[c.vid];
        cv.nrm = normals[c.vid];
        cv.orig_u = c.u;
        cv.orig_v = c.v;
        cv.skin = skin_weights[c.vid];
        cv.region = c.region;
        cv.flags = c.flags;
        cv.cut_mask = 0U;
        return cv;
    };

    const auto interp_clip_vert = [&](
        const ClipVert& a,
        const ClipVert& b,
        const float sa,
        const float sb,
        const std::size_t plane_idx) noexcept -> ClipVert {
        const float denom = sa - sb;
        const float t = (std::abs(denom) > 1.0e-7F) ? std::clamp(sa / denom, 0.0F, 1.0F) : 0.5F;
        ClipVert out{};
        out.base_p = glm::mix(a.base_p, b.base_p, t);
        const glm::vec3 mn = glm::mix(a.nrm, b.nrm, t);
        const float mlen = glm::length(mn);
        out.nrm = (mlen > 1.0e-6F) ? (mn / mlen) : a.nrm;
        out.orig_u = glm::mix(a.orig_u, b.orig_u, t);
        out.orig_v = glm::mix(a.orig_v, b.orig_v, t);
        out.skin = lerp_skin_vertex(a.skin, b.skin, t);
        out.region = (t < 0.5F) ? a.region : b.region;
        out.flags = (t < 0.5F) ? a.flags : b.flags;
        out.cut_mask = static_cast<std::uint8_t>((a.cut_mask & b.cut_mask) | (1U << plane_idx));
        return out;
    };

    for (std::size_t i = 0U; i + 2U < part.indices.size(); i += 3U) {
        const std::uint32_t i0 = part.indices[i + 0U];
        const std::uint32_t i1 = part.indices[i + 1U];
        const std::uint32_t i2 = part.indices[i + 2U];
        if (i0 >= part.corners.size() || i1 >= part.corners.size() || i2 >= part.corners.size()) {
            continue;
        }
        const TemplateCorner& c0 = part.corners[i0];
        const TemplateCorner& c1 = part.corners[i1];
        const TemplateCorner& c2 = part.corners[i2];
        if (!tri_allow_fn(c0, c1, c2)) {
            continue;
        }

        std::array<ClipVert, 8U> buf_a{};
        std::array<ClipVert, 8U> buf_b{};
        buf_a[0] = make_clip_vert(c0);
        buf_a[1] = make_clip_vert(c1);
        buf_a[2] = make_clip_vert(c2);
        std::size_t poly_n = 3U;

        for (std::size_t p = 0U; p < plane_count && poly_n >= 3U; ++p) {
            std::array<float, 8U> dists{};
            for (std::size_t k = 0U; k < poly_n; ++k) {
                dists[k] = plane_eval_fn(p, buf_a[k]);
                if (dists[k] >= 0.0F && dists[k] <= 1.0e-5F) {
                    buf_a[k].cut_mask = static_cast<std::uint8_t>(buf_a[k].cut_mask | (1U << p));
                }
            }
            std::size_t out_n = 0U;
            for (std::size_t k = 0U; k < poly_n; ++k) {
                const std::size_t kn = (k + 1U) % poly_n;
                const float sa = dists[k];
                const float sb = dists[kn];
                const bool in_a = (sa >= 0.0F);
                const bool in_b = (sb >= 0.0F);
                if (in_a) {
                    if (out_n < buf_b.size()) {
                        buf_b[out_n++] = buf_a[k];
                    }
                    if (!in_b && out_n < buf_b.size()) {
                        buf_b[out_n++] = interp_clip_vert(buf_a[k], buf_a[kn], sa, sb, p);
                    }
                } else if (in_b) {
                    if (out_n < buf_b.size()) {
                        buf_b[out_n++] = interp_clip_vert(buf_a[k], buf_a[kn], sa, sb, p);
                    }
                }
            }
            poly_n = out_n;
            for (std::size_t k = 0U; k < poly_n; ++k) {
                buf_a[k] = buf_b[k];
            }
        }

        if (poly_n < 3U) {
            continue;
        }

        std::array<std::uint32_t, 8U> out_vis{};
        std::array<glm::vec3, 8U> out_pos{};
        std::array<glm::vec2, 8U> out_uv{};
        for (std::size_t k = 0U; k < poly_n; ++k) {
            float min_d = (plane_count > 0U) ? 1.0e6F : 0.0F;
            for (std::size_t p = 0U; p < plane_count; ++p) {
                min_d = (std::min)(min_d, (std::max)(0.0F, plane_eval_fn(p, buf_a[k])));
            }
            out_pos[k] = offset_fn(buf_a[k], min_d);
            out_uv[k] = uv_fn(buf_a[k], out_pos[k]);
            mb.add_triangle_vertex(out_pos[k], buf_a[k].nrm, out_uv[k], buf_a[k].skin, target, out_vis[k]);
        }

        for (std::size_t k = 1U; k + 1U < poly_n; ++k) {
            const glm::vec3 cr = glm::cross(
                out_pos[k] - out_pos[0], out_pos[k + 1U] - out_pos[0]);
            if (glm::length(cr) <= 1.0e-9F) {
                continue;
            }
            mb.indices.push_back(out_vis[0]);
            mb.indices.push_back(out_vis[k]);
            mb.indices.push_back(out_vis[k + 1U]);
        }

        if (rim_depth > 1.0e-5F) {
            for (std::size_t k = 0U; k < poly_n; ++k) {
                const std::size_t kn = (k + 1U) % poly_n;
                if ((buf_a[k].cut_mask & buf_a[kn].cut_mask) == 0U) {
                    continue;
                }
                const glm::vec3& pa = out_pos[k];
                const glm::vec3& pb = out_pos[kn];
                if (glm::length(pb - pa) <= 1.0e-5F) {
                    continue;
                }
                const glm::vec3 pa_in = pa - buf_a[k].nrm * rim_depth;
                const glm::vec3 pb_in = pb - buf_a[kn].nrm * rim_depth;
                const glm::vec3 raw_wn = glm::cross(pa_in - pa, pb_in - pa);
                const float wlen = glm::length(raw_wn);
                const glm::vec3 wn = (wlen > 1.0e-7F) ? (raw_wn / wlen) : -buf_a[k].nrm;
                std::uint32_t r0 = 0U;
                std::uint32_t r1 = 0U;
                std::uint32_t r2 = 0U;
                std::uint32_t r3 = 0U;
                mb.add_triangle_vertex(pa, wn, out_uv[k], buf_a[k].skin, target, r0);
                mb.add_triangle_vertex(pa_in, wn, out_uv[k], buf_a[k].skin, target, r1);
                mb.add_triangle_vertex(pb_in, wn, out_uv[kn], buf_a[kn].skin, target, r2);
                mb.add_triangle_vertex(pb, wn, out_uv[kn], buf_a[kn].skin, target, r3);
                mb.indices.push_back(r0);
                mb.indices.push_back(r1);
                mb.indices.push_back(r2);
                mb.indices.push_back(r0);
                mb.indices.push_back(r2);
                mb.indices.push_back(r3);
            }
        }
    }
}

[[nodiscard]] std::uint8_t sample_skin_detail_alpha_nearest(
    const HumanBaseTemplate& tpl,
    const float u,
    const float v) noexcept {
    if (tpl.mask_size == 0U ||
        tpl.skin_detail_rgba.size() <
            static_cast<std::size_t>(tpl.mask_size) * static_cast<std::size_t>(tpl.mask_size) * 4U) {
        return 0U;
    }
    const std::uint32_t max_coord = tpl.mask_size - 1U;
    const auto px = static_cast<std::uint32_t>(std::clamp(u, 0.0F, 1.0F) * static_cast<float>(max_coord));
    const auto py = static_cast<std::uint32_t>(std::clamp(v, 0.0F, 1.0F) * static_cast<float>(max_coord));
    const std::size_t off = (static_cast<std::size_t>(py) * tpl.mask_size + px) * 4U;
    return tpl.skin_detail_rgba[off + 3U];
}

[[nodiscard]] float smoothstep_f(const float x, const float e0, const float e1) noexcept {
    const float t = std::clamp((x - e0) / (std::max)(1.0e-5F, e1 - e0), 0.0F, 1.0F);
    return t * t * (3.0F - 2.0F * t);
}

[[nodiscard]] float eval_anatomical_hairline_weight(
    const glm::vec3& pos,
    const CharacterGender gender,
    const glm::vec3& head_c,
    const float eye_y,
    const float neck_y,
    const float hs) noexcept {
    const float abs_x = std::abs(pos.x - head_c.x);
    // Exclude full outer ear pinna bounds
    if (abs_x > 0.0645F * hs &&
        pos.y > eye_y - 0.048F * hs &&
        pos.y < eye_y + 0.014F * hs &&
        pos.z > head_c.z - 0.028F * hs &&
        pos.z < head_c.z + 0.018F * hs) {
        return 0.0F;
    }
    // Exclude lower cheek/jaw/neck below ear lobe
    if (pos.y < eye_y - 0.052F * hs &&
        (pos.z > head_c.z - 0.006F * hs || abs_x > 0.055F * hs)) {
        return 0.0F;
    }

    const float front_z = head_c.z + 0.020F * hs;
    const float nape_w = smoothstep_f(pos.y, neck_y + 0.038F * hs, neck_y + 0.056F * hs);

    float cap_w = nape_w;
    if (pos.z > front_z) {
        const float blend_front = smoothstep_f(pos.z, front_z, front_z + 0.016F * hs);
        float trichion_y = eye_y + 0.056F * hs;
        if (gender == CharacterGender::female) {
            const float x_norm = std::clamp(abs_x / (0.055F * hs), 0.0F, 1.2F);
            trichion_y = eye_y + (0.054F + 0.004F * x_norm * x_norm) * hs;
        } else {
            const float temple_recess =
                std::exp(-std::pow((abs_x - 0.036F * hs) / (0.015F * hs), 2.0F));
            trichion_y = eye_y + (0.053F + 0.008F * temple_recess) * hs;
        }
        const float frontal_w = smoothstep_f(pos.y, trichion_y, trichion_y + 0.008F * hs);
        const float temple_arch_y = glm::mix(
            eye_y + 0.026F * hs,
            eye_y - 0.002F * hs,
            smoothstep_f(abs_x, 0.050F * hs, 0.064F * hs));
        const float temple_w = smoothstep_f(pos.y, temple_arch_y, temple_arch_y + 0.009F * hs);
        const float side_blend = smoothstep_f(abs_x, 0.047F * hs, 0.063F * hs);
        const float front_profile = glm::mix(frontal_w, temple_w, side_blend);
        cap_w = glm::mix(nape_w, front_profile, blend_front);
    } else if (abs_x >= 0.054F * hs && pos.z > head_c.z - 0.012F * hs) {
        const float temple_w = smoothstep_f(pos.y, eye_y - 0.006F * hs, eye_y + 0.006F * hs);
        cap_w = (std::min)(cap_w, temple_w);
    }
    return std::clamp(cap_w, 0.0F, 1.0F);
}

[[nodiscard]] float eval_anatomical_beard_weight(
    const glm::vec3& pos,
    const glm::vec3& head_c,
    const glm::vec3& mouth_c,
    const float chin_y,
    const float eye_y,
    const float hs) noexcept {
    const float dx = pos.x - mouth_c.x;
    const float abs_x = std::abs(dx);
    if (abs_x > 0.0690F * hs || pos.z < head_c.z + 0.012F * hs || pos.y < chin_y - 0.012F * hs) {
        return 0.0F;
    }
    if (pos.y < chin_y - 0.002F * hs && pos.z < head_c.z + 0.026F * hs) {
        return 0.0F;
    }
    const float lip_slit_y = mouth_c.y + 0.0023F * hs;
    const float dy_slit = pos.y - lip_slit_y;
    const float dz = pos.z - mouth_c.z;
    // Exclusion ellipse matched to true oral slit (lip_slit_y)
    const float lip_dist = std::hypot(dx / (0.0305F * hs), dy_slit / (0.0130F * hs));
    if (lip_dist < 1.04F && dz > -0.025F * hs) {
        return 0.0F;
    }
    // Softened sub-lip hollow beside the central soul-patch connector below lower lip
    const float hx =
        smoothstep_f(abs_x, 0.0065F * hs, 0.0105F * hs) *
        (1.0F - smoothstep_f(abs_x, 0.0185F * hs, 0.0235F * hs));
    const float hy =
        smoothstep_f(pos.y, lip_slit_y - 0.0195F * hs, lip_slit_y - 0.0155F * hs) *
        (1.0F - smoothstep_f(pos.y, lip_slit_y - 0.0115F * hs, lip_slit_y - 0.0085F * hs));
    const float hz = smoothstep_f(dz, -0.022F * hs, -0.014F * hs);
    const float sub_lip_atten = 1.0F - 0.52F * hx * hy * hz;

    const float rear_w = smoothstep_f(pos.z - (head_c.z + 0.012F * hs), 0.0F, 0.008F * hs);

    // Mustache bar strictly below nostrils
    const float must_top =
        mouth_c.y + (0.0222F - 0.0028F * (std::min)(1.0F, abs_x / (0.031F * hs))) * hs;
    if (abs_x <= 0.0315F * hs &&
        pos.y >= mouth_c.y + 0.0124F * hs &&
        pos.y <= must_top &&
        dz > -0.016F * hs) {
        const float top_feather = smoothstep_f(must_top - pos.y, 0.0F, 0.0018F * hs);
        return std::clamp(0.45F + 0.55F * top_feather, 0.0F, 1.0F);
    }
    // Oral corner connectors flanking the lip aperture
    if (abs_x >= 0.0245F * hs && abs_x <= 0.0385F * hs &&
        pos.y >= mouth_c.y - 0.014F * hs &&
        pos.y <= mouth_c.y + 0.016F * hs &&
        dz > -0.024F * hs) {
        return rear_w;
    }
    // Pre-auricular sideburn extending cleanly up to lower temple
    if (abs_x >= 0.0510F * hs &&
        abs_x <= 0.0690F * hs &&
        pos.z >= head_c.z + 0.012F * hs &&
        pos.z <= head_c.z + 0.064F * hs &&
        pos.y <= eye_y + 0.016F * hs &&
        pos.y >= mouth_c.y - 0.016F * hs) {
        const float sb_top = 1.0F - 0.50F * smoothstep_f(pos.y, eye_y + 0.006F * hs, eye_y + 0.016F * hs);
        return std::clamp(sb_top * rear_w, 0.0F, 1.0F);
    }

    const float cheek_upper_y =
        mouth_c.y - 0.003F * hs +
        0.024F * hs * smoothstep_f(abs_x, 0.028F * hs, 0.060F * hs);
    const float cheek_lin = smoothstep_f(cheek_upper_y - pos.y, -0.0040F * hs, 0.0100F * hs);
    const float cheek_w = cheek_lin * (0.35F + 0.65F * cheek_lin);
    return std::clamp((std::min)(cheek_w, rear_w) * sub_lip_atten, 0.0F, 1.0F);
}

[[nodiscard]] CharacterGroomInputs build_groom_inputs_from_template(
    const CharacterPackage& pkg,
    const HumanBaseTemplate& tpl,
    const EvaluatedHumanTemplate& eval) {
    CharacterGroomInputs in{};
    in.gender = pkg.body.gender;
    in.hair_style = pkg.wardrobe.hair_style;
    in.has_beard = pkg.wardrobe.has_beard;
    in.seed = pkg.identity.seed;
    const float h = pkg.body.height_meters / 1.78F;
    in.head_scale = pkg.body.face.head_scale * h;
    in.head_center = eval.bones[6].model_pos;
    in.neck_center = eval.bones[5].model_pos;
    in.eye_l = eval.bones[43].model_pos;
    in.eye_r = eval.bones[44].model_pos;
    in.jaw_center = eval.bones[38].model_pos;
    in.mouth_center = 0.5F * (eval.bones[39].model_pos + eval.bones[40].model_pos);
    in.body_positions = eval.positions;
    in.body_skin = tpl.skin_weights;
    in.landmark_chains = tpl.landmark_chains;

    const TemplatePart& body_part = tpl.parts[part_body];
    float crown_y = -1.0e6F;
    float chin_y = 1.0e6F;
    float sole_y = 1.0e6F;
    for (const TemplateCorner& c : body_part.corners) {
        const glm::vec3& p = eval.positions[c.vid];
        sole_y = (std::min)(sole_y, p.y);
        if (static_cast<TemplateRegion>(c.region) == TemplateRegion::head) {
            crown_y = (std::max)(crown_y, p.y);
            chin_y = (std::min)(chin_y, p.y);
        }
    }
    in.crown_y = (crown_y > -1.0e5F) ? crown_y : (in.head_center.y + 0.14F * in.head_scale);
    in.chin_y = (chin_y < 1.0e5F) ? chin_y : (in.jaw_center.y - 0.04F * in.head_scale);
    in.sole_y = (sole_y < 1.0e5F) ? sole_y : 0.0F;

    const float eye_y = 0.5F * (in.eye_l.y + in.eye_r.y);
    const float hs = in.head_scale;
    const bool collect_scalp =
        pkg.wardrobe.hair_style != HairStyle::bald &&
        pkg.wardrobe.hair_style != HairStyle::hood;

    in.scalp_triangles.reserve(1600U);
    if (pkg.wardrobe.has_beard) {
        in.beard_triangles.reserve(1200U);
    }

    for (std::size_t i = 0U; i + 2U < body_part.indices.size(); i += 3U) {
        const std::uint32_t i0 = body_part.indices[i + 0U];
        const std::uint32_t i1 = body_part.indices[i + 1U];
        const std::uint32_t i2 = body_part.indices[i + 2U];
        if (i0 >= body_part.corners.size() ||
            i1 >= body_part.corners.size() ||
            i2 >= body_part.corners.size()) {
            continue;
        }
        const TemplateCorner& c0 = body_part.corners[i0];
        const TemplateCorner& c1 = body_part.corners[i1];
        const TemplateCorner& c2 = body_part.corners[i2];
        const auto r0 = static_cast<TemplateRegion>(c0.region);
        const auto r1 = static_cast<TemplateRegion>(c1.region);
        const auto r2 = static_cast<TemplateRegion>(c2.region);

        const glm::vec3 p_mid =
            (eval.positions[c0.vid] + eval.positions[c1.vid] + eval.positions[c2.vid]) * (1.0F / 3.0F);

        const bool all_head_or_neck =
            (r0 == TemplateRegion::head || r0 == TemplateRegion::neck) &&
            (r1 == TemplateRegion::head || r1 == TemplateRegion::neck) &&
            (r2 == TemplateRegion::head || r2 == TemplateRegion::neck);
        if (collect_scalp && all_head_or_neck) {
            const float hl_mid = eval_anatomical_hairline_weight(
                p_mid, pkg.body.gender, in.head_center, eye_y, in.neck_center.y, hs);
            const float hl0 = eval_anatomical_hairline_weight(
                eval.positions[c0.vid], pkg.body.gender, in.head_center, eye_y, in.neck_center.y, hs);
            const float hl1 = eval_anatomical_hairline_weight(
                eval.positions[c1.vid], pkg.body.gender, in.head_center, eye_y, in.neck_center.y, hs);
            const float hl2 = eval_anatomical_hairline_weight(
                eval.positions[c2.vid], pkg.body.gender, in.head_center, eye_y, in.neck_center.y, hs);
            if (hl_mid >= 0.44F && (std::min)({hl0, hl1, hl2}) >= 0.10F) {
                in.scalp_triangles.push_back({c0.vid, c1.vid, c2.vid});
            }
        }

        if (pkg.wardrobe.has_beard &&
            r0 == TemplateRegion::head &&
            r1 == TemplateRegion::head &&
            r2 == TemplateRegion::head) {
            const float max_vy = (std::max)({eval.positions[c0.vid].y, eval.positions[c1.vid].y, eval.positions[c2.vid].y});
            const float min_vy = (std::min)({eval.positions[c0.vid].y, eval.positions[c1.vid].y, eval.positions[c2.vid].y});
            const float max_vz = (std::max)({eval.positions[c0.vid].z, eval.positions[c1.vid].z, eval.positions[c2.vid].z});
            if (min_vy < eye_y + 0.018F * hs &&
                max_vy > in.chin_y - 0.016F * hs &&
                max_vz > in.head_center.z - 0.012F * hs) {
                in.beard_triangles.push_back({c0.vid, c1.vid, c2.vid});
            }
        }
    }

    if (collect_scalp && in.scalp_triangles.empty()) {
        for (std::size_t i = 0U; i + 2U < body_part.indices.size(); i += 3U) {
            const std::uint32_t i0 = body_part.indices[i + 0U];
            const std::uint32_t i1 = body_part.indices[i + 1U];
            const std::uint32_t i2 = body_part.indices[i + 2U];
            if (i0 >= body_part.corners.size() ||
                i1 >= body_part.corners.size() ||
                i2 >= body_part.corners.size()) {
                continue;
            }
            const TemplateCorner& c0 = body_part.corners[i0];
            const TemplateCorner& c1 = body_part.corners[i1];
            const TemplateCorner& c2 = body_part.corners[i2];
            const auto r0 = static_cast<TemplateRegion>(c0.region);
            const auto r1 = static_cast<TemplateRegion>(c1.region);
            const auto r2 = static_cast<TemplateRegion>(c2.region);
            if ((r0 == TemplateRegion::head || r0 == TemplateRegion::neck) &&
                (r1 == TemplateRegion::head || r1 == TemplateRegion::neck) &&
                (r2 == TemplateRegion::head || r2 == TemplateRegion::neck)) {
                const glm::vec3 p_mid =
                    (eval.positions[c0.vid] + eval.positions[c1.vid] + eval.positions[c2.vid]) * (1.0F / 3.0F);
                if (p_mid.y > eye_y + 0.042F * hs) {
                    in.scalp_triangles.push_back({c0.vid, c1.vid, c2.vid});
                }
            }
        }
    }

    return in;
}

[[nodiscard]] glm::vec2 remap_body_corner_uv(const TemplateCorner& c) noexcept {
    if (static_cast<TemplateRegion>(c.region) == TemplateRegion::inner_mouth) {
        const float u_remapped = std::clamp(0.82F + (c.u - 0.62F) * 1.25F, 0.81F, 0.985F);
        const float v_remapped = std::clamp(0.015F + (c.v - 0.015F) * 0.70F, 0.012F, 0.098F);
        return {u_remapped, v_remapped};
    }
    return {c.u, c.v};
}

template <typename TriPred>
void emit_subdivided_body_part(
    MeshBuilder& mb,
    const TemplatePart& part,
    const std::span<const glm::vec3> positions,
    const std::span<const glm::vec3> welded_normals,
    const std::span<const CookedSkinVertex> skin_weights,
    const CharacterPaintTarget target,
    const TriPred& tri_pred) {
    if (part.corners.empty() || part.indices.size() < 3U) {
        return;
    }

    std::vector<std::uint32_t> kept_corner_indices{};
    kept_corner_indices.reserve(part.indices.size());
    for (std::size_t i = 0U; i + 2U < part.indices.size(); i += 3U) {
        const std::uint32_t i0 = part.indices[i + 0U];
        const std::uint32_t i1 = part.indices[i + 1U];
        const std::uint32_t i2 = part.indices[i + 2U];
        if (i0 >= part.corners.size() || i1 >= part.corners.size() || i2 >= part.corners.size()) {
            continue;
        }
        if (tri_pred(part.corners[i0], part.corners[i1], part.corners[i2])) {
            kept_corner_indices.push_back(i0);
            kept_corner_indices.push_back(i1);
            kept_corner_indices.push_back(i2);
        }
    }
    if (kept_corner_indices.empty()) {
        return;
    }

    struct WeldedEdgeInfo {
        glm::vec3 pos{0.0F};
        glm::vec3 nrm{0.0F, 1.0F, 0.0F};
        CookedSkinVertex skin{};
        std::uint32_t weld_slot = 0U;
    };

    const std::uint32_t v_start = static_cast<std::uint32_t>(mb.vertices.size());
    const std::uint32_t i_start = static_cast<std::uint32_t>(mb.indices.size());
    const std::size_t kept_tris = kept_corner_indices.size() / 3U;

    std::vector<std::uint32_t> corner_to_mesh(
        part.corners.size(), (std::numeric_limits<std::uint32_t>::max)());
    std::unordered_map<std::uint64_t, WeldedEdgeInfo> welded_edges;
    welded_edges.reserve(kept_tris * 2U);
    std::unordered_map<std::uint64_t, std::uint32_t> corner_mid_to_mesh;
    corner_mid_to_mesh.reserve(kept_tris * 2U);
    std::vector<std::uint32_t> mesh_v_weld_slot;
    mesh_v_weld_slot.reserve(kept_tris * 2U + part.corners.size());

    std::uint32_t next_weld_slot = static_cast<std::uint32_t>(positions.size());

    const auto get_corner_vertex = [&](const std::uint32_t ic) -> std::uint32_t {
        if (corner_to_mesh[ic] != (std::numeric_limits<std::uint32_t>::max)()) {
            return corner_to_mesh[ic];
        }
        const TemplateCorner& c = part.corners[ic];
        std::uint32_t out_v = 0U;
        mb.add_triangle_vertex(
            positions[c.vid], welded_normals[c.vid], remap_body_corner_uv(c),
            skin_weights[c.vid], target, out_v);
        corner_to_mesh[ic] = out_v;
        mesh_v_weld_slot.push_back(static_cast<std::uint32_t>(c.vid));
        return out_v;
    };

    const auto get_edge_midpoint_vertex = [&](const std::uint32_t ic_a,
                                              const std::uint32_t ic_b) -> std::uint32_t {
        const std::uint32_t ci_lo = (std::min)(ic_a, ic_b);
        const std::uint32_t ci_hi = (std::max)(ic_a, ic_b);
        const std::uint64_t corner_key =
            (static_cast<std::uint64_t>(ci_lo) << 32U) | static_cast<std::uint64_t>(ci_hi);
        if (const auto it = corner_mid_to_mesh.find(corner_key); it != corner_mid_to_mesh.end()) {
            return it->second;
        }

        const TemplateCorner& ca = part.corners[ic_a];
        const TemplateCorner& cb = part.corners[ic_b];
        const std::uint16_t v_lo = (std::min)(ca.vid, cb.vid);
        const std::uint16_t v_hi = (std::max)(ca.vid, cb.vid);
        const std::uint64_t v_key =
            (static_cast<std::uint64_t>(v_lo) << 16U) | static_cast<std::uint64_t>(v_hi);

        auto w_it = welded_edges.find(v_key);
        if (w_it == welded_edges.end()) {
            const glm::vec3& pa = positions[v_lo];
            const glm::vec3& pb = positions[v_hi];
            const glm::vec3& na = welded_normals[v_lo];
            const glm::vec3& nb = welded_normals[v_hi];
            const glm::vec3 d = pb - pa;
            const float crease_w = glm::smoothstep(0.25F, 0.75F, glm::dot(na, nb));
            const float convex_t = glm::smoothstep(-0.004F, 0.004F, glm::dot(d, nb - na));
            const float pn_strength = glm::mix(0.024F, 0.095F, convex_t);
            const glm::vec3 p_mid = 0.5F * (pa + pb) -
                (pn_strength * crease_w) * (glm::dot(d, na) * na - glm::dot(d, nb) * nb);
            const glm::vec3 n_sum = na + nb;
            const glm::vec3 n_mid = glm::length(n_sum) > 1.0e-6F ? glm::normalize(n_sum) : na;
            const CookedSkinVertex sk_mid =
                lerp_skin_vertex(skin_weights[v_lo], skin_weights[v_hi], 0.5F);
            WeldedEdgeInfo info{p_mid, n_mid, sk_mid, next_weld_slot++};
            w_it = welded_edges.emplace(v_key, info).first;
        }

        const glm::vec2 uv_mid = 0.5F * (remap_body_corner_uv(ca) + remap_body_corner_uv(cb));
        std::uint32_t out_v = 0U;
        mb.add_triangle_vertex(
            w_it->second.pos, w_it->second.nrm, uv_mid,
            w_it->second.skin, target, out_v);
        corner_mid_to_mesh.emplace(corner_key, out_v);
        mesh_v_weld_slot.push_back(w_it->second.weld_slot);
        return out_v;
    };

    mb.vertices.reserve(mb.vertices.size() + kept_tris * 2U);
    mb.skin_vertices.reserve(mb.skin_vertices.size() + kept_tris * 2U);
    mb.vertex_paint_targets.reserve(mb.vertex_paint_targets.size() + kept_tris * 2U);
    mb.indices.reserve(mb.indices.size() + kept_tris * 12U);

    for (std::size_t i = 0U; i + 2U < kept_corner_indices.size(); i += 3U) {
        const std::uint32_t c0 = kept_corner_indices[i + 0U];
        const std::uint32_t c1 = kept_corner_indices[i + 1U];
        const std::uint32_t c2 = kept_corner_indices[i + 2U];

        const std::uint32_t v0 = get_corner_vertex(c0);
        const std::uint32_t v1 = get_corner_vertex(c1);
        const std::uint32_t v2 = get_corner_vertex(c2);
        const std::uint32_t e01 = get_edge_midpoint_vertex(c0, c1);
        const std::uint32_t e12 = get_edge_midpoint_vertex(c1, c2);
        const std::uint32_t e20 = get_edge_midpoint_vertex(c2, c0);

        mb.indices.push_back(v0);
        mb.indices.push_back(e01);
        mb.indices.push_back(e20);

        mb.indices.push_back(v1);
        mb.indices.push_back(e12);
        mb.indices.push_back(e01);

        mb.indices.push_back(v2);
        mb.indices.push_back(e20);
        mb.indices.push_back(e12);

        mb.indices.push_back(e01);
        mb.indices.push_back(e12);
        mb.indices.push_back(e20);
    }

    // Recompute smooth area-weighted normals per welded 3D vertex slot (crack-free across UV seams)
    std::vector<glm::vec3> weld_normals(next_weld_slot, glm::vec3(0.0F));
    for (std::size_t ii = i_start; ii + 2U < mb.indices.size(); ii += 3U) {
        const std::uint32_t ia = mb.indices[ii + 0U];
        const std::uint32_t ib = mb.indices[ii + 1U];
        const std::uint32_t ic = mb.indices[ii + 2U];
        const glm::vec3 pa(mb.vertices[ia].position[0], mb.vertices[ia].position[1], mb.vertices[ia].position[2]);
        const glm::vec3 pb(mb.vertices[ib].position[0], mb.vertices[ib].position[1], mb.vertices[ib].position[2]);
        const glm::vec3 pc(mb.vertices[ic].position[0], mb.vertices[ic].position[1], mb.vertices[ic].position[2]);
        const glm::vec3 fn = glm::cross(pb - pa, pc - pa);
        weld_normals[mesh_v_weld_slot[ia - v_start]] += fn;
        weld_normals[mesh_v_weld_slot[ib - v_start]] += fn;
        weld_normals[mesh_v_weld_slot[ic - v_start]] += fn;
    }
    for (std::size_t k = 0U; k < mesh_v_weld_slot.size(); ++k) {
        const glm::vec3& accum = weld_normals[mesh_v_weld_slot[k]];
        const float len = glm::length(accum);
        if (len > 1.0e-7F) {
            const glm::vec3 nn = accum / len;
            mb.vertices[v_start + k].normal = {nn.x, nn.y, nn.z};
        }
    }
}

void recompute_uv_derivative_tangents_for_submeshes(MeshBuilder& mb) {
    const std::size_t vcount = mb.vertices.size();
    if (vcount == 0U) {
        return;
    }
    std::vector<glm::vec3> tan1(vcount, glm::vec3(0.0F));
    std::vector<glm::vec3> tan2(vcount, glm::vec3(0.0F));
    std::vector<std::uint8_t> touched(vcount, 0U);

    for (const CharacterSubmeshBuild& sub : mb.submeshes) {
        if (sub.name == "hair") {
            continue;
        }
        const std::size_t i_end = (std::min)(
            mb.indices.size(),
            static_cast<std::size_t>(sub.first_index) + sub.index_count);
        for (std::size_t ii = sub.first_index; ii + 2U < i_end; ii += 3U) {
            const std::uint32_t ia = mb.indices[ii + 0U];
            const std::uint32_t ib = mb.indices[ii + 1U];
            const std::uint32_t ic = mb.indices[ii + 2U];
            if (ia >= vcount || ib >= vcount || ic >= vcount) {
                continue;
            }
            touched[ia] = 1U;
            touched[ib] = 1U;
            touched[ic] = 1U;

            const CookedModelVertex& va = mb.vertices[ia];
            const CookedModelVertex& vb = mb.vertices[ib];
            const CookedModelVertex& vc = mb.vertices[ic];

            const glm::vec3 pa(va.position[0], va.position[1], va.position[2]);
            const glm::vec3 pb(vb.position[0], vb.position[1], vb.position[2]);
            const glm::vec3 pc(vc.position[0], vc.position[1], vc.position[2]);
            const glm::vec3 dp1 = pb - pa;
            const glm::vec3 dp2 = pc - pa;

            // Unflip texcoord.y back into PNG image UV space matching editor_model.vert
            const float du1 = vb.texcoord[0] - va.texcoord[0];
            const float dv1 = (1.0F - vb.texcoord[1]) - (1.0F - va.texcoord[1]);
            const float du2 = vc.texcoord[0] - va.texcoord[0];
            const float dv2 = (1.0F - vc.texcoord[1]) - (1.0F - va.texcoord[1]);

            const float det = du1 * dv2 - du2 * dv1;
            if (std::abs(det) <= 1.0e-10F) {
                continue;
            }
            const float inv = 1.0F / det;
            const glm::vec3 sdir = (dp1 * dv2 - dp2 * dv1) * inv;
            const glm::vec3 tdir = (dp2 * du1 - dp1 * du2) * inv;
            if (!std::isfinite(sdir.x) || !std::isfinite(tdir.x) ||
                glm::length(sdir) > 1.0e5F || glm::length(tdir) > 1.0e5F) {
                continue;
            }
            tan1[ia] += sdir;
            tan1[ib] += sdir;
            tan1[ic] += sdir;
            tan2[ia] += tdir;
            tan2[ib] += tdir;
            tan2[ic] += tdir;
        }
    }

    for (std::size_t vi = 0U; vi < vcount; ++vi) {
        if (touched[vi] == 0U) {
            continue;
        }
        CookedModelVertex& v = mb.vertices[vi];
        const glm::vec3 raw_n(v.normal[0], v.normal[1], v.normal[2]);
        const glm::vec3 n = glm::length(raw_n) > 1.0e-6F
            ? glm::normalize(raw_n)
            : glm::vec3(0.0F, 1.0F, 0.0F);
        v.normal = {n.x, n.y, n.z};

        const glm::vec3 t_raw = tan1[vi];
        const glm::vec3 t_ortho = t_raw - n * glm::dot(n, t_raw);
        if (glm::dot(t_ortho, t_ortho) > 1.0e-10F) {
            const glm::vec3 t = glm::normalize(t_ortho);
            const float w = (glm::dot(glm::cross(n, t), tan2[vi]) < 0.0F) ? -1.0F : 1.0F;
            v.tangent = {t.x, t.y, t.z, w};
        } else {
            const glm::vec3 up = std::abs(n.y) < 0.95F
                ? glm::vec3(0.0F, 1.0F, 0.0F)
                : glm::vec3(1.0F, 0.0F, 0.0F);
            const glm::vec3 t = glm::normalize(glm::cross(up, n));
            v.tangent = {t.x, t.y, t.z, 1.0F};
        }
    }
}

}  // namespace

CharacterMeshBuildResult build_character_mesh(const CharacterPackage& input_pkg) {
    CharacterPackage pkg = input_pkg;
    sanitize_character_package(pkg);

    CharacterMeshBuildResult result{};
    result.skeleton = build_character_skeleton(pkg);

    MeshBuilder mb{};
    const auto begin_submesh = [&](const char* name,
                                   const CharacterPaintTarget target,
                                   const PbrMaterial& mat) {
        CharacterSubmeshBuild sub{};
        sub.name = name;
        sub.paint_target = target;
        sub.first_index = static_cast<std::uint32_t>(mb.indices.size());
        sub.material_index = static_cast<std::uint32_t>(mb.submeshes.size());
        sub.material = mat;
        return sub;
    };
    const auto end_submesh = [&](CharacterSubmeshBuild sub) {
        const auto cur_indices = static_cast<std::uint32_t>(mb.indices.size());
        if (cur_indices > sub.first_index) {
            sub.index_count = cur_indices - sub.first_index;
            mb.submeshes.push_back(std::move(sub));
        }
    };

    const HumanBaseTemplate* const tpl = get_human_base_template(pkg.body.gender);
    if (tpl != nullptr) {
        const EvaluatedHumanTemplate eval = evaluate_human_template(pkg.body, *tpl);
        const auto& bones = eval.bones;
        const float h = pkg.body.height_meters / 1.78F;
        const float hs = pkg.body.face.head_scale * h;

        const std::vector<glm::vec3> skin_normals = compute_part_welded_normals(
            tpl->parts[part_body], eval.positions);
        const std::vector<glm::vec3> eyes_normals = compute_part_welded_normals(
            tpl->parts[part_eyes], eval.positions);
        const std::vector<glm::vec3> tights_normals = compute_part_welded_normals(
            tpl->parts[part_tights], eval.positions);
        const std::vector<glm::vec3> skirt_normals = compute_part_welded_normals(
            tpl->parts[part_skirt], eval.positions);

        // ---- Submesh 0: Anatomical Human Skin & Inner Mouth ----
        {
            PbrMaterial skin_mat{};
            skin_mat.base_color = glm::vec4(pkg.body.skin_color, 1.0F);
            skin_mat.metallic = 0.0F;
            skin_mat.roughness = pkg.body.skin_roughness;
            skin_mat.ior = 1.38F;
            skin_mat.ambient_occlusion = 1.0F;
            skin_mat.model = MaterialModel::subsurface;
            skin_mat.subsurface_profile = SubsurfaceProfile::skin;
            skin_mat.subsurface_strength = pkg.body.skin_subsurface_strength;
            auto skin_sub = begin_submesh(
                "skin", CharacterPaintTarget::skin, sanitize_pbr_material(skin_mat));

            const TemplatePart& body_part = tpl->parts[part_body];
            std::vector<std::uint8_t> ring1_uncovered(
                tpl->vertex_count, pkg.wardrobe.occlude_covered_body ? 0U : 1U);
            if (pkg.wardrobe.occlude_covered_body) {
                std::vector<std::uint8_t> touches_uncovered(tpl->vertex_count, 0U);
                const float neck_y_ref = bones[5].model_pos.y;
                const float chest_y_ref = bones[4].model_pos.y;
                const float head_z_ref = bones[6].model_pos.z;
                for (const TemplateCorner& c : body_part.corners) {
                    const auto reg = static_cast<TemplateRegion>(c.region);
                    bool uncovered = !is_region_covered_by_wardrobe(pkg.wardrobe, reg);
                    if (!uncovered && reg == TemplateRegion::torso_upper) {
                        const glm::vec3& vp = eval.positions[c.vid];
                        const bool collar_ring = vp.y >= neck_y_ref - 0.065F * h;
                        const bool front_vneck_chest =
                            vp.y >= chest_y_ref - 0.015F * h &&
                            std::abs(vp.x) <= 0.105F * h &&
                            vp.z >= head_z_ref + 0.010F * h;
                        if (collar_ring || front_vneck_chest) {
                            uncovered = true;
                        }
                    }
                    if (uncovered) {
                        touches_uncovered[c.vid] = 1U;
                    }
                }
                // Keep a conservative 2-ring border of skin vertices adjacent to exposed regions
                // so V-neck collars, short sleeves, shorts, and boot cuffs have zero floating gaps.
                ring1_uncovered = touches_uncovered;
                for (int ring = 0; ring < 2; ++ring) {
                    std::vector<std::uint8_t> next_ring = ring1_uncovered;
                    for (std::size_t i = 0U; i + 2U < body_part.indices.size(); i += 3U) {
                        const std::uint16_t v0 = body_part.corners[body_part.indices[i + 0U]].vid;
                        const std::uint16_t v1 = body_part.corners[body_part.indices[i + 1U]].vid;
                        const std::uint16_t v2 = body_part.corners[body_part.indices[i + 2U]].vid;
                        if (ring1_uncovered[v0] != 0U ||
                            ring1_uncovered[v1] != 0U ||
                            ring1_uncovered[v2] != 0U) {
                            next_ring[v0] = 1U;
                            next_ring[v1] = 1U;
                            next_ring[v2] = 1U;
                        }
                    }
                    ring1_uncovered = std::move(next_ring);
                }
            }

            const std::uint32_t skin_v_start = static_cast<std::uint32_t>(mb.vertices.size());
            const std::uint32_t skin_i_start = static_cast<std::uint32_t>(mb.indices.size());
            emit_subdivided_body_part(
                mb,
                body_part,
                eval.positions,
                skin_normals,
                tpl->skin_weights,
                CharacterPaintTarget::skin,
                [&](const TemplateCorner& c0, const TemplateCorner& c1, const TemplateCorner& c2) noexcept {
                    return ring1_uncovered[c0.vid] != 0U ||
                           ring1_uncovered[c1.vid] != 0U ||
                           ring1_uncovered[c2.vid] != 0U;
                });

            if (!pkg.sculpt_strokes.empty() && mb.vertices.size() > skin_v_start) {
                apply_sculpt_strokes_to_mesh(
                    pkg.sculpt_strokes, mb.vertices, mb.vertices.size());
                recompute_vertex_normals_range(mb, skin_v_start, skin_i_start);
            }
            end_submesh(std::move(skin_sub));
        }

        // ---- Submesh 1: Two-Shell Glossy Eyes (Inner Iris/Sclera + Outer Cornea Dome) ----
        {
            PbrMaterial eye_mat{};
            eye_mat.base_color = glm::vec4(1.0F);
            eye_mat.metallic = 0.0F;
            eye_mat.roughness = 0.22F;
            eye_mat.ior = 1.40F;
            eye_mat.model = MaterialModel::standard;
            auto eye_sub = begin_submesh(
                "eyes", CharacterPaintTarget::skin, sanitize_pbr_material(eye_mat));

            const std::uint32_t eye_v_start = static_cast<std::uint32_t>(mb.vertices.size());
            const std::uint32_t eye_i_start = static_cast<std::uint32_t>(mb.indices.size());
            emit_template_part_subset(
                mb,
                tpl->parts[part_eyes],
                eval.positions,
                eyes_normals,
                0.0F,
                CharacterPaintTarget::skin,
                [](const TemplateCorner&, const TemplateCorner&, const TemplateCorner&) noexcept {
                    return true;
                },
                [hs](const TemplateCorner& c, const glm::vec3& p, const glm::vec3& n) noexcept {
                    const float rad = std::hypot((c.u - 0.5F) * 2.0F, (c.v - 0.5F) * 2.0F);
                    if (rad < 0.465F) {
                        const float k = rad / 0.465F;
                        const float concave = 1.0F - k * k;
                        return p - n * (0.00085F * hs * concave);
                    }
                    return p;
                },
                [](const TemplateCorner& c, const glm::vec3&) noexcept {
                    return glm::vec2(c.u, c.v);
                },
                [&](const TemplateCorner& c, const glm::vec3&) noexcept {
                    return tpl->skin_weights[c.vid];
                });
            recompute_vertex_normals_range(mb, eye_v_start, eye_i_start);
            end_submesh(std::move(eye_sub));

            PbrMaterial cornea_mat{};
            cornea_mat.base_color = glm::vec4(1.0F, 1.0F, 1.0F, 0.032F);
            cornea_mat.metallic = 0.0F;
            cornea_mat.roughness = 0.022F;
            cornea_mat.ior = 1.376F;
            cornea_mat.model = MaterialModel::standard;
            auto cornea_sub = begin_submesh(
                "cornea", CharacterPaintTarget::skin, sanitize_pbr_material(cornea_mat));

            const std::uint32_t cornea_v_start = static_cast<std::uint32_t>(mb.vertices.size());
            const std::uint32_t cornea_i_start = static_cast<std::uint32_t>(mb.indices.size());
            emit_template_part_subset(
                mb,
                tpl->parts[part_eyes],
                eval.positions,
                eyes_normals,
                0.0F,
                CharacterPaintTarget::skin,
                [&](const TemplateCorner& c0, const TemplateCorner& c1, const TemplateCorner& c2) noexcept {
                    const auto front_eye = [&](const TemplateCorner& c) noexcept {
                        const glm::vec3& p = eval.positions[c.vid];
                        const glm::vec3& eye_c =
                            (p.x >= 0.0F) ? bones[43].model_pos : bones[44].model_pos;
                        return p.z > eye_c.z - 0.002F * hs;
                    };
                    return front_eye(c0) || front_eye(c1) || front_eye(c2);
                },
                [hs](const TemplateCorner& c, const glm::vec3& p, const glm::vec3& n) noexcept {
                    const float rad = std::hypot((c.u - 0.5F) * 2.0F, (c.v - 0.5F) * 2.0F);
                    const float dome = std::pow(std::clamp((0.48F - rad) / 0.48F, 0.0F, 1.0F), 1.4F);
                    return p + n * ((0.00028F + 0.00095F * dome) * hs);
                },
                [](const TemplateCorner& c, const glm::vec3&) noexcept {
                    return glm::vec2(c.u, c.v);
                },
                [&](const TemplateCorner& c, const glm::vec3&) noexcept {
                    return tpl->skin_weights[c.vid];
                });
            recompute_vertex_normals_range(mb, cornea_v_start, cornea_i_start);
            end_submesh(std::move(cornea_sub));
        }

        // ---- Submeshes 2 & 3: Modular 3D Tailored Wardrobe (Primary & Secondary) ----
        {
            CharacterWardrobeBuildInputs wardrobe_in{};
            wardrobe_in.package = &pkg;
            wardrobe_in.positions = eval.positions;
            wardrobe_in.body_normals = skin_normals;
            wardrobe_in.tights_normals = tights_normals;
            wardrobe_in.skirt_normals = skirt_normals;
            wardrobe_in.skin_weights = tpl->skin_weights;
            for (std::size_t b = 0U; b < character_total_bone_count; ++b) {
                wardrobe_in.bone_positions[b] = bones[b].model_pos;
            }
            wardrobe_in.body_part = WardrobeTemplatePartView{
                tpl->parts[part_body].corners, tpl->parts[part_body].indices};
            wardrobe_in.tights_part = WardrobeTemplatePartView{
                tpl->parts[part_tights].corners, tpl->parts[part_tights].indices};
            wardrobe_in.skirt_part = WardrobeTemplatePartView{
                tpl->parts[part_skirt].corners, tpl->parts[part_skirt].indices};

            const CharacterWardrobeMeshOutputs wardrobe_out =
                build_character_wardrobe_meshes(wardrobe_in);

            const auto append_wardrobe_slot = [&](
                const char* slot_name,
                const CharacterPaintTarget target,
                const WardrobeSlotMesh& slot) {
                if (slot.vertices.empty() || slot.indices.size() < 3U) {
                    return;
                }
                auto sub = begin_submesh(
                    slot_name, target, sanitize_pbr_material(slot.material));
                const auto v_base = static_cast<std::uint32_t>(mb.vertices.size());
                const std::size_t n = slot.vertices.size();

                mb.vertices.insert(
                    mb.vertices.end(), slot.vertices.begin(), slot.vertices.end());
                if (slot.skin_vertices.size() == n) {
                    mb.skin_vertices.reserve(mb.skin_vertices.size() + n);
                    for (CookedSkinVertex sk : slot.skin_vertices) {
                        sanitize_skin_vertex(sk, character_total_bone_count);
                        mb.skin_vertices.push_back(sk);
                    }
                } else {
                    mb.skin_vertices.resize(mb.vertices.size(), CookedSkinVertex{});
                }
                mb.vertex_paint_targets.resize(
                    mb.vertices.size(), static_cast<std::uint8_t>(target));

                mb.vertex_garment_zones.resize(
                    v_base, static_cast<std::uint8_t>(GarmentZoneId::none));
                mb.vertex_garment_ao.resize(v_base, 1.0F);
                mb.vertex_garment_curv.resize(v_base, 0.5F);

                if (slot.vertex_zones.size() == n) {
                    mb.vertex_garment_zones.insert(
                        mb.vertex_garment_zones.end(),
                        slot.vertex_zones.begin(),
                        slot.vertex_zones.end());
                } else {
                    mb.vertex_garment_zones.resize(
                        mb.vertices.size(), static_cast<std::uint8_t>(GarmentZoneId::none));
                }
                if (slot.vertex_ao.size() == n) {
                    mb.vertex_garment_ao.insert(
                        mb.vertex_garment_ao.end(),
                        slot.vertex_ao.begin(),
                        slot.vertex_ao.end());
                } else {
                    mb.vertex_garment_ao.resize(mb.vertices.size(), 1.0F);
                }
                if (slot.vertex_curv.size() == n) {
                    mb.vertex_garment_curv.insert(
                        mb.vertex_garment_curv.end(),
                        slot.vertex_curv.begin(),
                        slot.vertex_curv.end());
                } else {
                    mb.vertex_garment_curv.resize(mb.vertices.size(), 0.5F);
                }

                mb.indices.reserve(mb.indices.size() + slot.indices.size());
                for (std::size_t ii = 0U; ii + 2U < slot.indices.size(); ii += 3U) {
                    const std::uint32_t i0 = slot.indices[ii + 0U];
                    const std::uint32_t i1 = slot.indices[ii + 1U];
                    const std::uint32_t i2 = slot.indices[ii + 2U];
                    if (i0 < n && i1 < n && i2 < n) {
                        mb.indices.push_back(v_base + i0);
                        mb.indices.push_back(v_base + i1);
                        mb.indices.push_back(v_base + i2);
                    }
                }
                end_submesh(std::move(sub));
            };

            append_wardrobe_slot(
                "outfit_primary", CharacterPaintTarget::outfit_primary, wardrobe_out.primary);
            append_wardrobe_slot(
                "outfit_secondary", CharacterPaintTarget::outfit_secondary, wardrobe_out.secondary);
        }

        // ---- Submesh 4: Vendor-Agnostic 3D Micro-Strand Hair, Eyebrows, Eyelashes & Beard ----
        {
            const CharacterGroomInputs groom_in =
                build_groom_inputs_from_template(pkg, *tpl, eval);
            const CharacterStrandGroomMesh groom =
                build_character_strand_groom_mesh(groom_in);
            if (!groom.indices.empty() && !groom.vertices.empty()) {
                PbrMaterial hair_mat{};
                hair_mat.base_color = glm::vec4(1.0F);
                hair_mat.metallic = 0.0F;
                hair_mat.roughness = 0.32F;
                hair_mat.ior = 1.55F;
                hair_mat.ambient_occlusion = 1.0F;
                hair_mat.model = MaterialModel::strand_hair;
                auto hair_sub = begin_submesh(
                    "hair", CharacterPaintTarget::hair, sanitize_pbr_material(hair_mat));

                const auto hair_v_start = static_cast<std::uint32_t>(mb.vertices.size());
                mb.vertices.insert(mb.vertices.end(), groom.vertices.begin(), groom.vertices.end());
                mb.skin_vertices.insert(
                    mb.skin_vertices.end(), groom.skin_vertices.begin(), groom.skin_vertices.end());
                mb.vertex_paint_targets.resize(
                    mb.vertices.size(), static_cast<std::uint8_t>(CharacterPaintTarget::hair));
                mb.vertex_garment_zones.resize(
                    mb.vertices.size(), static_cast<std::uint8_t>(GarmentZoneId::none));
                mb.vertex_garment_ao.resize(mb.vertices.size(), 1.0F);
                mb.vertex_garment_curv.resize(mb.vertices.size(), 0.5F);
                mb.indices.reserve(mb.indices.size() + groom.indices.size());
                for (const std::uint32_t idx : groom.indices) {
                    mb.indices.push_back(hair_v_start + idx);
                }

                end_submesh(std::move(hair_sub));
            }
        }

        recompute_uv_derivative_tangents_for_submeshes(mb);
    } else {
        // Resilient minimal fallback when template files are absent outside engine root.
        const auto& bones = fallback_character_bone_layout(pkg.body);
        const float h = pkg.body.height_meters / 1.78F;
        PbrMaterial skin_mat{};
        skin_mat.base_color = glm::vec4(pkg.body.skin_color, 1.0F);
        skin_mat.roughness = pkg.body.skin_roughness;
        skin_mat.ior = 1.38F;
        skin_mat.model = MaterialModel::subsurface;
        skin_mat.subsurface_profile = SubsurfaceProfile::skin;
        skin_mat.subsurface_strength = pkg.body.skin_subsurface_strength;
        auto skin_sub = begin_submesh(
            "skin", CharacterPaintTarget::skin, sanitize_pbr_material(skin_mat));
        const std::array<LoftRing, 4U> fallback_rings{{
            {bones[1].model_pos, {1,0,0}, {0,0,1}, 0.14F * h, 0.11F * h, 0.0F, {{1U, 0U, 0U, 0U}, {1.0F, 0, 0, 0}}},
            {bones[2].model_pos, {1,0,0}, {0,0,1}, 0.12F * h, 0.10F * h, 0.35F, {{2U, 1U, 0U, 0U}, {0.7F, 0.3F, 0, 0}}},
            {bones[4].model_pos, {1,0,0}, {0,0,1}, 0.17F * h, 0.12F * h, 0.75F, {{4U, 3U, 0U, 0U}, {0.8F, 0.2F, 0, 0}}},
            {bones[6].model_pos + glm::vec3(0, 0.14F * h, 0), {1,0,0}, {0,0,1}, 0.08F * h, 0.09F * h, 1.0F, {{6U, 0U, 0U, 0U}, {1.0F, 0, 0, 0}}},
        }};
        emit_loft_tube(
            mb, fallback_rings, 12U, {0.10F, 0.10F}, {0.90F, 0.90F},
            CharacterPaintTarget::skin, true, true);
        emit_ellipsoid(
            mb, bones[6].model_pos + glm::vec3(0, 0.09F * h, 0),
            {0.095F * h, 0.115F * h, 0.105F * h}, 8U, 12U,
            {0.10F, 0.10F}, {0.90F, 0.45F}, {{6U, 0U, 0U, 0U}, {1.0F, 0, 0, 0}},
            CharacterPaintTarget::skin);
        end_submesh(std::move(skin_sub));
        recompute_uv_derivative_tangents_for_submeshes(mb);
    }

    mb.vertex_garment_zones.resize(
        mb.vertices.size(), static_cast<std::uint8_t>(GarmentZoneId::none));
    mb.vertex_garment_ao.resize(mb.vertices.size(), 1.0F);
    mb.vertex_garment_curv.resize(mb.vertices.size(), 0.5F);

    result.vertices = std::move(mb.vertices);
    result.skin_vertices = std::move(mb.skin_vertices);
    result.indices = std::move(mb.indices);
    result.submeshes = std::move(mb.submeshes);
    result.vertex_paint_targets = std::move(mb.vertex_paint_targets);
    result.vertex_garment_zones = std::move(mb.vertex_garment_zones);
    result.vertex_garment_ao = std::move(mb.vertex_garment_ao);
    result.vertex_garment_curv = std::move(mb.vertex_garment_curv);

    if (!result.vertices.empty()) {
        glm::vec3 mn(
            result.vertices.front().position[0],
            result.vertices.front().position[1],
            result.vertices.front().position[2]);
        glm::vec3 mx = mn;
        for (const CookedModelVertex& v : result.vertices) {
            mn.x = (std::min)(mn.x, v.position[0]);
            mn.y = (std::min)(mn.y, v.position[1]);
            mn.z = (std::min)(mn.z, v.position[2]);
            mx.x = (std::max)(mx.x, v.position[0]);
            mx.y = (std::max)(mx.y, v.position[1]);
            mx.z = (std::max)(mx.z, v.position[2]);
        }
        result.bounds_min = mn;
        result.bounds_max = mx;
    }

    return result;
}

// ============================================================================
// Layered PBR Texture Baker (sRGB BaseColor, Linear Normal & MetallicRoughness)
// ============================================================================

namespace {

[[nodiscard]] float worley_pores_2d(
    const float x,
    const float y,
    const std::uint32_t seed) noexcept {
    const int gx = static_cast<int>(std::floor(x));
    const int gy = static_cast<int>(std::floor(y));
    float min_d2 = 4.0F;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            const auto cx = static_cast<std::uint32_t>(gx + dx + 4096);
            const auto cy = static_cast<std::uint32_t>(gy + dy + 4096);
            const float jx = static_cast<float>(gx + dx) + 0.18F +
                0.64F * hash_float01(cx, cy, seed);
            const float jy = static_cast<float>(gy + dy) + 0.18F +
                0.64F * hash_float01(cx, cy, seed ^ 0x9e3779b9U);
            const float ddx = jx - x;
            const float ddy = jy - y;
            min_d2 = (std::min)(min_d2, ddx * ddx + ddy * ddy);
        }
    }
    return std::exp(-min_d2 * 14.0F);
}

struct SkinUvCell {
    glm::vec3 pos{0.0F};
    std::uint8_t region = 0U;
    std::uint8_t valid = 0U;
};

[[nodiscard]] std::vector<SkinUvCell> rasterize_body_uv_grid(
    const HumanBaseTemplate& tpl,
    const EvaluatedHumanTemplate& eval,
    const std::uint32_t res) {
    const std::size_t total = static_cast<std::size_t>(res) * res;
    std::vector<SkinUvCell> grid(total);
    const TemplatePart& body_part = tpl.parts[part_body];
    const float fres = static_cast<float>(res);

    for (std::size_t i = 0U; i + 2U < body_part.indices.size(); i += 3U) {
        const std::uint32_t i0 = body_part.indices[i + 0U];
        const std::uint32_t i1 = body_part.indices[i + 1U];
        const std::uint32_t i2 = body_part.indices[i + 2U];
        if (i0 >= body_part.corners.size() ||
            i1 >= body_part.corners.size() ||
            i2 >= body_part.corners.size()) {
            continue;
        }
        const TemplateCorner& c0 = body_part.corners[i0];
        const TemplateCorner& c1 = body_part.corners[i1];
        const TemplateCorner& c2 = body_part.corners[i2];
        const glm::vec2 uv0 = remap_body_corner_uv(c0);
        const glm::vec2 uv1 = remap_body_corner_uv(c1);
        const glm::vec2 uv2 = remap_body_corner_uv(c2);

        const float denom =
            (uv1.y - uv2.y) * (uv0.x - uv2.x) + (uv2.x - uv1.x) * (uv0.y - uv2.y);
        if (std::abs(denom) <= 1.0e-10F) {
            continue;
        }
        const float inv_denom = 1.0F / denom;

        const float min_u = (std::min)(uv0.x, (std::min)(uv1.x, uv2.x));
        const float max_u = (std::max)(uv0.x, (std::max)(uv1.x, uv2.x));
        const float min_v = (std::min)(uv0.y, (std::min)(uv1.y, uv2.y));
        const float max_v = (std::max)(uv0.y, (std::max)(uv1.y, uv2.y));

        const int x0 = std::clamp(static_cast<int>(std::floor(min_u * fres)) - 1, 0, static_cast<int>(res) - 1);
        const int x1 = std::clamp(static_cast<int>(std::ceil(max_u * fres)), 0, static_cast<int>(res) - 1);
        const int y0 = std::clamp(static_cast<int>(std::floor(min_v * fres)) - 1, 0, static_cast<int>(res) - 1);
        const int y1 = std::clamp(static_cast<int>(std::ceil(max_v * fres)), 0, static_cast<int>(res) - 1);

        const glm::vec3& p0 = eval.positions[c0.vid];
        const glm::vec3& p1 = eval.positions[c1.vid];
        const glm::vec3& p2 = eval.positions[c2.vid];
        const float inv_res = 1.0F / fres;

        for (int py = y0; py <= y1; ++py) {
            const float pv = (static_cast<float>(py) + 0.5F) * inv_res;
            for (int px = x0; px <= x1; ++px) {
                const float pu = (static_cast<float>(px) + 0.5F) * inv_res;
                const float w0 = ((uv1.y - uv2.y) * (pu - uv2.x) + (uv2.x - uv1.x) * (pv - uv2.y)) * inv_denom;
                const float w1 = ((uv2.y - uv0.y) * (pu - uv2.x) + (uv0.x - uv2.x) * (pv - uv2.y)) * inv_denom;
                const float w2 = 1.0F - w0 - w1;
                if (w0 < -0.004F || w1 < -0.004F || w2 < -0.004F) {
                    continue;
                }
                const float bw0 = (std::max)(0.0F, w0);
                const float bw1 = (std::max)(0.0F, w1);
                const float bw2 = (std::max)(0.0F, w2);
                const float wsum = bw0 + bw1 + bw2;
                const float inv_w = wsum > 1.0e-6F ? (1.0F / wsum) : 1.0F;
                const glm::vec3 pos = (p0 * bw0 + p1 * bw1 + p2 * bw2) * inv_w;
                const std::uint8_t reg = (bw0 >= bw1 && bw0 >= bw2)
                    ? c0.region
                    : ((bw1 >= bw2) ? c1.region : c2.region);
                grid[static_cast<std::size_t>(py) * res + static_cast<std::size_t>(px)] =
                    SkinUvCell{pos, reg, 1U};
            }
        }
    }

    // 3-pass UV gutter dilation so bilinear seam borders sample clean 3D coordinates/regions
    std::vector<SkinUvCell> next_grid = grid;
    for (int pass = 0; pass < 3; ++pass) {
        bool any_filled = false;
        for (int py = 0; py < static_cast<int>(res); ++py) {
            for (int px = 0; px < static_cast<int>(res); ++px) {
                const std::size_t idx =
                    static_cast<std::size_t>(py) * res + static_cast<std::size_t>(px);
                if (grid[idx].valid != 0U) {
                    continue;
                }
                glm::vec3 sum_pos(0.0F);
                int count = 0;
                std::uint8_t first_reg = 0U;
                for (int dy = -1; dy <= 1; ++dy) {
                    const int ny = py + dy;
                    if (ny < 0 || ny >= static_cast<int>(res)) continue;
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = px + dx;
                        if ((dx == 0 && dy == 0) || nx < 0 || nx >= static_cast<int>(res)) continue;
                        const SkinUvCell& nb =
                            grid[static_cast<std::size_t>(ny) * res + static_cast<std::size_t>(nx)];
                        if (nb.valid != 0U) {
                            if (pass > 0 && nb.region == static_cast<std::uint8_t>(TemplateRegion::inner_mouth)) {
                                continue;
                            }
                            sum_pos += nb.pos;
                            if (count == 0) first_reg = nb.region;
                            ++count;
                        }
                    }
                }
                if (count > 0) {
                    next_grid[idx] = SkinUvCell{sum_pos / static_cast<float>(count), first_reg, 1U};
                    any_filled = true;
                }
            }
        }
        grid.swap(next_grid);
        next_grid = grid;
        if (!any_filled) {
            break;
        }
    }
    return grid;
}

} // namespace

std::vector<BakedPbrTextureSet> bake_character_textures(
    const CharacterPackage& input_pkg,
    const CharacterMeshBuildResult& mesh,
    const std::uint32_t requested_res) {
    CharacterPackage pkg = input_pkg;
    sanitize_character_package(pkg);
    const std::uint32_t res = std::clamp(requested_res, 64U, 1024U);
    std::size_t pixel_count = 0U;
    std::size_t byte_count = 0U;
    if (!checked_mul_size(res, res, pixel_count) ||
        !checked_mul_size(pixel_count, 4U, byte_count)) {
        return {};
    }

    const HumanBaseTemplate* const tpl = get_human_base_template(pkg.body.gender);
    bool need_skin_raster = false;
    bool need_groom_inputs = false;
    for (const CharacterSubmeshBuild& sub : mesh.submeshes) {
        if (sub.name == "skin") need_skin_raster = true;
        if (sub.name == "hair") need_groom_inputs = true;
    }

    EvaluatedHumanTemplate eval{};
    bool has_eval = false;
    std::vector<SkinUvCell> skin_uv_grid{};
    if (tpl != nullptr && (need_skin_raster || need_groom_inputs)) {
        eval = evaluate_human_template(pkg.body, *tpl);
        has_eval = true;
        if (need_skin_raster) {
            skin_uv_grid = rasterize_body_uv_grid(*tpl, eval, res);
        }
    }

    const float h = pkg.body.height_meters / 1.78F;
    const float hs = pkg.body.face.head_scale * h;
    const glm::vec3 head_c = has_eval ? eval.bones[6].model_pos : glm::vec3(0.0F, 1.62F * h, 0.04F * h);
    const float eye_y = has_eval
        ? (0.5F * (eval.bones[43].model_pos.y + eval.bones[44].model_pos.y))
        : (head_c.y + 0.04F * hs);
    const float neck_y = has_eval ? eval.bones[5].model_pos.y : (head_c.y - 0.085F * hs);
    const glm::vec3 mouth_c = has_eval
        ? 0.5F * (eval.bones[39].model_pos + eval.bones[40].model_pos)
        : glm::vec3(0.0F, eye_y - 0.074F * hs, head_c.z + 0.115F * hs);
    float chin_y = mouth_c.y - 0.050F * hs;
    if (has_eval && tpl != nullptr) {
        float min_head_y = 1.0e6F;
        for (const TemplateCorner& c : tpl->parts[part_body].corners) {
            if (static_cast<TemplateRegion>(c.region) == TemplateRegion::head) {
                min_head_y = (std::min)(min_head_y, eval.positions[c.vid].y);
            }
        }
        if (min_head_y < 1.0e5F) {
            chin_y = min_head_y;
        }
    }

    std::vector<BakedPbrTextureSet> outputs;
    outputs.reserve(mesh.submeshes.size());

    for (const CharacterSubmeshBuild& sub : mesh.submeshes) {
        if (sub.name == "outfit_primary" || sub.name == "outfit_secondary") {
            outputs.push_back(bake_character_wardrobe_textures(pkg, mesh, sub, res));
            continue;
        }
        if (sub.name == "hair" && tpl != nullptr && has_eval) {
            const CharacterGroomInputs groom_in =
                build_groom_inputs_from_template(pkg, *tpl, eval);
            outputs.push_back(bake_character_strand_groom_textures(
                groom_in,
                pkg.wardrobe.hair_color,
                pkg.wardrobe.wear_and_tear,
                pkg.paint_strokes,
                (std::min)(512U, res),
                (std::min)(512U, res)));
            continue;
        }

        if (sub.name == "cornea") {
            BakedPbrTextureSet cset{};
            cset.slot_name = sub.name;
            cset.target = sub.paint_target;
            constexpr std::uint32_t c_res = 64U;
            cset.width = c_res;
            cset.height = c_res;
            const std::size_t c_bytes = static_cast<std::size_t>(c_res) * c_res * 4U;
            cset.base_color_rgba.resize(c_bytes);
            cset.normal_rgba.resize(c_bytes);
            cset.metallic_roughness_rgba.resize(c_bytes);
            for (std::size_t px = 0U; px < static_cast<std::size_t>(c_res) * c_res; ++px) {
                const std::size_t off = px * 4U;
                cset.base_color_rgba[off + 0U] = 255U;
                cset.base_color_rgba[off + 1U] = 255U;
                cset.base_color_rgba[off + 2U] = 255U;
                cset.base_color_rgba[off + 3U] = 255U;
                cset.normal_rgba[off + 0U] = 128U;
                cset.normal_rgba[off + 1U] = 128U;
                cset.normal_rgba[off + 2U] = 255U;
                cset.normal_rgba[off + 3U] = 255U;
                cset.metallic_roughness_rgba[off + 0U] = 255U;
                cset.metallic_roughness_rgba[off + 1U] = 255U;
                cset.metallic_roughness_rgba[off + 2U] = 0U;
                cset.metallic_roughness_rgba[off + 3U] = 255U;
            }
            outputs.push_back(std::move(cset));
            continue;
        }

        std::vector<CharacterPaintStroke> slot_strokes;
        if (sub.name != "eyes") {
            for (const CharacterPaintStroke& ps : pkg.paint_strokes) {
                if (ps.target == sub.paint_target) {
                    slot_strokes.push_back(ps);
                }
            }
        }

        BakedPbrTextureSet set{};
        set.slot_name = sub.name;
        set.target = sub.paint_target;
        set.width = res;
        set.height = res;
        set.base_color_rgba.resize(byte_count);
        set.normal_rgba.resize(byte_count);
        set.metallic_roughness_rgba.resize(byte_count);

        const float inv_res = 1.0F / static_cast<float>(res);
        const glm::vec3 base_skin_lin = glm::vec3(
            srgb_to_linear_channel(pkg.body.skin_color.r),
            srgb_to_linear_channel(pkg.body.skin_color.g),
            srgb_to_linear_channel(pkg.body.skin_color.b)) * 0.96F;
        const glm::vec3 hair_lin(
            srgb_to_linear_channel(pkg.wardrobe.hair_color.r),
            srgb_to_linear_channel(pkg.wardrobe.hair_color.g),
            srgb_to_linear_channel(pkg.wardrobe.hair_color.b));
        const glm::vec3 eye_lin(
            srgb_to_linear_channel(pkg.body.face.eye_color.r),
            srgb_to_linear_channel(pkg.body.face.eye_color.g),
            srgb_to_linear_channel(pkg.body.face.eye_color.b));

        for (std::uint32_t y = 0U; y < res; ++y) {
            const float v = (static_cast<float>(y) + 0.5F) * inv_res;
            for (std::uint32_t x = 0U; x < res; ++x) {
                const float u = (static_cast<float>(x) + 0.5F) * inv_res;
                glm::vec3 col{1.0F};
                glm::vec3 nrm{0.0F, 0.0F, 1.0F};
                float metallic = sub.material.metallic;
                float roughness = sub.material.roughness;
                float ao = 1.0F;

                if (sub.name == "eyes") {
                    const float dx = (u - 0.5F) * 2.0F;
                    const float dy = (v - 0.5F) * 2.0F;
                    const float rad = std::hypot(dx, dy);
                    if (rad < 0.152F) {
                        const float edge = std::clamp((0.152F - rad) / 0.016F, 0.0F, 1.0F);
                        col = glm::mix(eye_lin * 0.10F, glm::vec3(0.006F, 0.006F, 0.008F), edge);
                        roughness = 0.08F;
                    } else if (rad < 0.435F) {
                        const float ang = std::atan2(dy, dx);
                        const float radial_t = (rad - 0.152F) / (0.435F - 0.152F);
                        const float n1 = value_noise_2d(std::cos(ang) * 7.0F + 4.0F, std::sin(ang) * 7.0F + radial_t * 3.0F, 31U);
                        const float n2 = value_noise_2d(std::cos(ang) * 18.0F + 9.0F, std::sin(ang) * 18.0F + radial_t * 6.0F, 47U);
                        const float fiber = 0.72F + 0.18F * std::sin(ang * 32.0F + (n1 - 0.5F) * 5.0F) +
                                            0.12F * std::sin(ang * 74.0F + (n2 - 0.5F) * 4.0F);
                        const float crypt = 0.86F + 0.22F * n2;
                        const float collarette = 0.84F + 0.28F * std::exp(-std::pow((radial_t - 0.28F) * 4.2F, 2.0F));
                        const float limbal = std::clamp((0.435F - rad) / 0.085F, 0.06F, 1.0F);
                        const glm::vec3 inner_ochre = eye_lin * glm::vec3(1.22F, 0.94F, 0.68F);
                        const glm::vec3 iris_base = glm::mix(
                            inner_ochre, eye_lin * glm::vec3(0.95F, 1.00F, 1.05F),
                            std::clamp(radial_t * 1.4F, 0.0F, 1.0F));
                        col = iris_base * (fiber * crypt * collarette * limbal);
                        // Concave conical iris stroma normal reaction
                        const float dir_len = (std::max)(rad, 1.0e-4F);
                        const float tilt = 0.18F * (1.0F - radial_t * 0.45F);
                        nrm = glm::normalize(glm::vec3(
                            -(dx / dir_len) * tilt,
                            -(dy / dir_len) * tilt,
                            1.0F));
                        roughness = 0.18F + (1.0F - n2) * 0.08F;
                    } else {
                        const float edge_ao = std::clamp(1.0F - (rad - 0.435F) * 0.42F, 0.52F, 1.0F);
                        const float v_ang = std::atan2(dy, dx);
                        const float vein_n = value_noise_2d(u * 26.0F, v * 26.0F, 71U);
                        const float vein = std::pow(
                            std::clamp(std::abs(std::sin(v_ang * 14.0F + vein_n * 6.0F)), 0.0F, 1.0F),
                            14.0F) * std::clamp((rad - 0.50F) / 0.24F, 0.0F, 1.0F);
                        const float corner_caruncle =
                            std::clamp((std::abs(dx) - 0.68F) / 0.24F, 0.0F, 1.0F) *
                            std::clamp(1.0F - std::abs(dy) * 1.8F, 0.0F, 1.0F);
                        glm::vec3 sclera = glm::vec3(0.68F, 0.65F, 0.63F) * edge_ao;
                        sclera = glm::mix(sclera, glm::vec3(0.66F, 0.34F, 0.32F), vein * 0.26F);
                        sclera = glm::mix(sclera, glm::vec3(0.60F, 0.28F, 0.26F), corner_caruncle * 0.42F);
                        col = sclera;
                        roughness = 0.14F;
                    }
                    // Soft upper and lower eyelid ambient shadow
                    const float upper_lid_ao = std::clamp(0.50F + (dy + 0.34F) * 0.78F, 0.40F, 1.0F);
                    const float lower_lid_ao = std::clamp(1.0F - (std::max)(0.0F, dy - 0.32F) * 0.55F, 0.68F, 1.0F);
                    col *= upper_lid_ao * lower_lid_ao;
                } else if (sub.name == "skin") {
                    const std::size_t cell_idx = static_cast<std::size_t>(y) * res + x;
                    const SkinUvCell cell = (!skin_uv_grid.empty())
                        ? skin_uv_grid[cell_idx]
                        : SkinUvCell{};
                    const auto reg = static_cast<TemplateRegion>(cell.region);
                    const bool is_head = (cell.valid != 0U) && (reg == TemplateRegion::head);
                    const bool is_mouth = (cell.valid != 0U) && (reg == TemplateRegion::inner_mouth);

                    if (is_mouth) {
                        if (u >= 0.8825F && v >= 0.0521F) {
                            const float upper_bias = (v >= 0.0703F) ? 1.0F : 0.90F;
                            col = glm::vec3(0.76F, 0.73F, 0.67F) * upper_bias;
                            roughness = 0.24F;
                        } else {
                            col = glm::vec3(0.38F, 0.13F, 0.13F);
                            roughness = 0.34F;
                        }
                    } else {
                        // Worley cellular micro-pores + fine dermal micro-grain
                        const float pore_scale = is_head ? 210.0F : 140.0F;
                        const float p_c = worley_pores_2d(u * pore_scale, v * pore_scale, 17U);
                        const float p_u = worley_pores_2d((u + 0.0008F) * pore_scale, v * pore_scale, 17U);
                        const float p_v = worley_pores_2d(u * pore_scale, (v + 0.0008F) * pore_scale, 17U);
                        const float grain = value_noise_2d(u * 380.0F, v * 380.0F, 59U) - 0.5F;
                        const float age = (value_noise_2d(u * 26.0F, v * 26.0F, 41U) - 0.5F) * pkg.body.age_wear;

                        const float t_zone = is_head
                            ? (std::clamp(1.0F - std::abs(cell.pos.x) / (0.048F * hs), 0.0F, 1.0F) *
                               std::clamp((cell.pos.z - (head_c.z + 0.045F * hs)) / (0.055F * hs), 0.0F, 1.0F))
                            : 0.0F;
                        const float pore_weight = is_head ? (0.45F + 0.55F * t_zone) : 0.35F;

                        col = base_skin_lin * (1.0F - p_c * 0.032F * pore_weight + grain * 0.020F + age * 0.10F);
                        const float dh_du = -(p_u - p_c) * pore_weight * 0.14F;
                        const float dh_dv = -(p_v - p_c) * pore_weight * 0.14F;
                        nrm = glm::normalize(glm::vec3(-dh_du, -dh_dv, 1.0F));
                        roughness = std::clamp(
                            pkg.body.skin_roughness + (is_head ? -0.025F * t_zone : 0.06F) + p_c * 0.05F,
                            0.44F,
                            0.92F);

                        // 3D Anatomical Vascular / Melanin Color Zones
                        if (is_head) {
                            const float py_rel = cell.pos.y - eye_y;
                            const float forehead_w = std::clamp((py_rel - 0.020F * hs) / (0.040F * hs), 0.0F, 1.0F);
                            const float midface_w =
                                std::clamp((py_rel + 0.055F * hs) / (0.040F * hs), 0.0F, 1.0F) *
                                (1.0F - std::clamp((py_rel - 0.012F * hs) / (0.035F * hs), 0.0F, 1.0F));
                            const float jaw_w = std::clamp((-py_rel - 0.045F * hs) / (0.045F * hs), 0.0F, 1.0F);
                            const float ear_w = std::clamp((std::abs(cell.pos.x) - 0.064F * hs) / (0.014F * hs), 0.0F, 1.0F);
                            const float nose_tip =
                                std::clamp((cell.pos.z - (head_c.z + 0.088F * hs)) / (0.025F * hs), 0.0F, 1.0F) *
                                std::clamp(1.0F - std::abs(py_rel + 0.024F * hs) / (0.028F * hs), 0.0F, 1.0F) *
                                std::clamp(1.0F - std::abs(cell.pos.x) / (0.020F * hs), 0.0F, 1.0F);

                            col = glm::mix(col, col * glm::vec3(1.03F, 1.00F, 0.94F), forehead_w * 0.45F);
                            col = glm::mix(col, base_skin_lin * glm::vec3(1.10F, 0.84F, 0.80F),
                                (std::min)(1.0F, midface_w * 0.22F + ear_w * 0.28F + nose_tip * 0.26F));
                            col = glm::mix(col, col * glm::vec3(0.96F, 0.97F, 0.95F), jaw_w * 0.35F);
                            if (pkg.body.gender == CharacterGender::male && jaw_w > 0.05F && !pkg.wardrobe.has_beard) {
                                // Subtle sub-dermal hair follicle cool shadow on shaven lower face
                                col = glm::mix(col, base_skin_lin * glm::vec3(0.88F, 0.91F, 0.95F), jaw_w * 0.14F);
                            }
                        } else if (cell.valid != 0U &&
                                   (reg == TemplateRegion::hand || reg == TemplateRegion::foot)) {
                            col = glm::mix(col, base_skin_lin * glm::vec3(1.06F, 0.89F, 0.85F), 0.18F);
                        }

                        if (tpl != nullptr && !is_mouth) {
                            const glm::vec4 mask = sample_skin_mask_bilinear(*tpl, u, v);
                            const glm::vec4 detail = sample_skin_detail_bilinear(*tpl, u, v);
                            const std::uint8_t detail_a = sample_skin_detail_alpha_nearest(*tpl, u, v);

                            // Baked v2 16-ray AO & discrete mean curvature
                            float detail_ao = detail.r;
                            if (is_head && cell.valid != 0U) {
                                const float py_ey = cell.pos.y - eye_y;
                                const float under_eye_soft =
                                    smoothstep_f(py_ey, -0.036F * hs, -0.020F * hs) *
                                    (1.0F - smoothstep_f(py_ey, -0.007F * hs, -0.001F * hs)) *
                                    (1.0F - smoothstep_f(std::abs(cell.pos.x), 0.036F * hs, 0.052F * hs)) *
                                    smoothstep_f(cell.pos.z, head_c.z + 0.038F * hs, head_c.z + 0.062F * hs) *
                                    (1.0F - std::clamp(pkg.body.age_wear * 2.2F, 0.0F, 0.85F));
                                detail_ao = glm::mix(detail_ao, 1.0F, 0.62F * under_eye_soft);
                            }
                            ao = std::clamp(glm::mix(1.0F, detail_ao, 0.84F), 0.24F, 1.0F);
                            const float crevice = std::clamp((0.68F - detail_ao) * 1.45F, 0.0F, 1.0F);
                            col *= glm::mix(glm::vec3(1.0F), glm::vec3(0.96F, 0.78F, 0.72F), crevice * 0.55F);
                            if (detail.g > 0.54F) {
                                roughness = (std::max)(0.42F, roughness - (detail.g - 0.54F) * 0.10F);
                            }

                            // Vermilion lips with crisp 3D anatomical border + fine sulcus grooves
                            if (is_head && cell.valid != 0U &&
                                cell.pos.z > mouth_c.z - 0.015F * hs && mask.r > 0.30F) {
                                const float lip_slit_y = mouth_c.y + 0.0023F * hs;
                                const float abs_lx =
                                    std::abs(cell.pos.x - mouth_c.x) / (std::max)(1.0e-4F, 0.0265F * hs);
                                if (abs_lx < 1.06F) {
                                    const float corner_env =
                                        std::sqrt((std::max)(0.0F, 1.0F - (std::min)(1.0F, abs_lx * abs_lx)));
                                    const float cupid =
                                        1.0F +
                                        0.14F * std::exp(-std::pow((abs_lx - 0.24F) / 0.16F, 2.0F)) -
                                        0.12F * std::exp(-std::pow(abs_lx / 0.10F, 2.0F));
                                    const float full_scale = 0.88F + 0.24F * pkg.body.face.lip_fullness;
                                    const float upper_h = 0.0098F * hs * corner_env * cupid * full_scale;
                                    const float lower_h =
                                        0.0106F * hs * std::pow(corner_env, 0.85F) * full_scale;
                                    const float dy_slit = cell.pos.y - lip_slit_y;
                                    const float ly_norm = (dy_slit >= 0.0F)
                                        ? (dy_slit / (std::max)(1.0e-4F, upper_h))
                                        : (-dy_slit / (std::max)(1.0e-4F, lower_h));
                                    const float vermilion =
                                        (1.0F - smoothstep_f(ly_norm, 0.74F, 1.02F)) *
                                        smoothstep_f(mask.r, 0.55F, 0.82F);
                                    if (vermilion > 0.005F) {
                                        const glm::vec3 lip_tint_col =
                                            base_skin_lin * glm::vec3(0.92F, 0.49F, 0.47F) +
                                            glm::vec3(0.030F, 0.007F, 0.009F) * pkg.body.lip_tint;
                                        const float lt = std::clamp(
                                            vermilion * (0.28F + pkg.body.lip_tint * 0.54F), 0.0F, 0.68F);
                                        col = glm::mix(col, lip_tint_col, lt);
                                        roughness = glm::mix(roughness, 0.34F, lt);
                                        const float lip_wrinkle = std::sin(
                                            u * 460.0F + value_noise_2d(u * 90.0F, v * 90.0F, 103U) * 4.0F);
                                        nrm = glm::normalize(glm::vec3(
                                            nrm.x + lip_wrinkle * lt * 0.13F, nrm.y, nrm.z));
                                    }
                                    if (std::abs(dy_slit) < 0.0018F * hs && abs_lx < 0.96F) {
                                        const float slit_t =
                                            (1.0F - std::abs(dy_slit) / (0.0018F * hs)) * corner_env;
                                        col *= (1.0F - 0.42F * slit_t);
                                        ao *= (1.0F - 0.25F * slit_t);
                                    }
                                }
                            }

                            // Under-3D-groom soft follicular brow shadow & moist lash tearline
                            if (mask.g > 0.01F) {
                                const glm::vec3 brow_col = glm::mix(
                                    base_skin_lin * 0.46F,
                                    hair_lin * 1.35F + glm::vec3(0.025F, 0.018F, 0.013F),
                                    0.48F);
                                const float bw = std::clamp(mask.g * 0.34F, 0.0F, 0.34F);
                                col = glm::mix(col, brow_col, bw);
                                roughness = glm::mix(roughness, 0.64F, bw);
                            }
                            if (mask.b > 0.01F) {
                                col *= (1.0F - mask.b * 0.38F);
                                roughness = glm::mix(roughness, 0.28F, mask.b * 0.65F);
                            }
                            if (mask.a > 0.01F) {
                                const float blush = mask.a * 0.18F;
                                col = glm::mix(col, base_skin_lin * glm::vec3(1.10F, 0.72F, 0.66F), blush);
                                if (pkg.body.freckles > 0.02F) {
                                    const float fdot = hash_float01(x / 2U, y / 2U, 99U);
                                    if (fdot > 1.0F - pkg.body.freckles * 0.14F * mask.a) {
                                        col *= glm::vec3(0.66F, 0.54F, 0.46F);
                                    }
                                }
                            }

                            // Keratin fingernails & toenails (raw alpha 1..127)
                            if (detail_a >= 1U && detail_a <= 127U && cell.valid != 0U &&
                                (reg == TemplateRegion::hand || reg == TemplateRegion::foot)) {
                                const float nail_w = std::clamp(static_cast<float>(detail_a) / 90.0F, 0.0F, 1.0F);
                                const glm::vec3 nail_pink = glm::mix(
                                    base_skin_lin * glm::vec3(1.06F, 0.88F, 0.86F),
                                    glm::vec3(0.76F, 0.58F, 0.54F),
                                    0.45F);
                                const float lunula = (detail_a < 72U) ? 0.18F : 0.0F;
                                const glm::vec3 nail_col = glm::mix(nail_pink, glm::vec3(0.84F, 0.80F, 0.76F), lunula);
                                col = glm::mix(col, nail_col, nail_w);
                                roughness = glm::mix(roughness, 0.21F, nail_w);
                            } else if (cell.valid != 0U &&
                                       (reg == TemplateRegion::head || reg == TemplateRegion::neck) &&
                                       pkg.wardrobe.hair_style != HairStyle::bald &&
                                       pkg.wardrobe.hair_style != HairStyle::hood) {
                                // Cranial scalp follicle root bed gated across complete hairline
                                const float hl_w = eval_anatomical_hairline_weight(
                                    cell.pos, pkg.body.gender, head_c, eye_y, neck_y, hs);
                                const bool is_short_crop = (pkg.wardrobe.hair_style == HairStyle::short_crop);
                                const float scalp_w = is_short_crop
                                    ? (smoothstep_f(hl_w, 0.18F, 0.76F) * 0.68F)
                                    : (smoothstep_f(hl_w, 0.10F, 0.60F) * 0.92F);
                                if (scalp_w > 0.005F) {
                                    const glm::vec3 root_tone = is_short_crop
                                        ? glm::mix(col * 0.30F, hair_lin * 0.82F, 0.74F)
                                        : glm::mix(col * 0.18F, hair_lin * 0.55F, 0.82F);
                                    col = glm::mix(col, root_tone, scalp_w);
                                    roughness = glm::mix(roughness, 0.78F, scalp_w);
                                    if (!is_short_crop) {
                                        ao = glm::mix(ao, 0.26F, scalp_w * 0.85F);
                                    }
                                }
                            }

                            // Subtle follicular shadow bed under 3D beard strands + fine pore stipple
                            if (is_head && pkg.wardrobe.has_beard && cell.valid != 0U) {
                                const float bw_3d = eval_anatomical_beard_weight(
                                    cell.pos, head_c, mouth_c, chin_y, eye_y, hs);
                                if (bw_3d > 0.05F) {
                                    const float pore_stipple = smoothstep_f(
                                        value_noise_2d(u * 440.0F, v * 440.0F, 711U) * 0.65F +
                                        hash_float01(x / 2U, y / 2U, 733U) * 0.35F,
                                        0.38F, 0.85F);
                                    const float bw_shaped = bw_3d * bw_3d;
                                    const float beard_shadow = bw_shaped * (0.22F + 0.16F * pore_stipple);
                                    const glm::vec3 follicle_tone = glm::mix(
                                        col * 0.58F,
                                        hair_lin * 0.62F + glm::vec3(0.012F, 0.009F, 0.006F),
                                        0.44F);
                                    col = glm::mix(col, follicle_tone, beard_shadow);
                                    roughness = glm::mix(roughness, 0.78F, beard_shadow);
                                    ao = glm::mix(ao, 0.68F, bw_shaped * 0.45F);
                                }
                            }
                        }
                    }
                } else {
                    const bool is_underwear_primary =
                        (sub.paint_target == CharacterPaintTarget::outfit_primary &&
                         pkg.wardrobe.studio_underwear &&
                         pkg.wardrobe.top_style == TopGarmentStyle::none);
                    const bool is_underwear_secondary =
                        (sub.paint_target == CharacterPaintTarget::outfit_secondary &&
                         pkg.wardrobe.studio_underwear &&
                         pkg.wardrobe.bottom_style == BottomGarmentStyle::none &&
                         v < 0.695F);
                    if (is_underwear_primary || is_underwear_secondary) {
                        const glm::vec3 cloth_lin(0.020F, 0.022F, 0.026F);
                        const glm::vec3 trim_lin(0.036F, 0.039F, 0.045F);
                        const float fgrain_c = value_noise_2d(u * 48.0F, v * 48.0F, 137U) - 0.5F;
                        const float fgrain_f = value_noise_2d(u * 130.0F, v * 130.0F, 139U) - 0.5F;
                        const float rib = 0.965F + 0.035F * std::sin(v * 260.0F);
                        const float waistband_trim = is_underwear_secondary
                            ? std::clamp(1.0F - std::abs(v - 0.075F) / 0.038F, 0.0F, 1.0F)
                            : std::clamp(1.0F - std::abs(v - 0.585F) / 0.038F, 0.0F, 1.0F);
                        col = glm::mix(cloth_lin, trim_lin, waistband_trim) *
                            (rib + (fgrain_c * 0.65F + fgrain_f * 0.35F) * 0.12F);
                        metallic = 0.0F;
                        roughness = 0.94F;
                        ao = 1.0F;
                        nrm = glm::normalize(glm::vec3(fgrain_f * 0.06F, std::cos(v * 260.0F) * 0.05F, 1.0F));
                    } else {
                        const glm::vec3 c_main = (sub.paint_target == CharacterPaintTarget::outfit_primary)
                            ? pkg.wardrobe.primary_color
                            : pkg.wardrobe.secondary_color;
                        const glm::vec3 main_lin(
                            srgb_to_linear_channel(c_main.r),
                            srgb_to_linear_channel(c_main.g),
                            srgb_to_linear_channel(c_main.b));
                        const glm::vec3 acc_lin(
                            srgb_to_linear_channel(pkg.wardrobe.accent_color.r),
                            srgb_to_linear_channel(pkg.wardrobe.accent_color.g),
                            srgb_to_linear_channel(pkg.wardrobe.accent_color.b));
                        const glm::vec3 boot_lin(
                            srgb_to_linear_channel(pkg.wardrobe.footwear_color.r),
                            srgb_to_linear_channel(pkg.wardrobe.footwear_color.g),
                            srgb_to_linear_channel(pkg.wardrobe.footwear_color.b));

                        // Smooth anti-aliased multi-scale cloth/leather micro-grain (no Nyquist moire rings)
                        const float grain_coarse = value_noise_2d(u * 42.0F, v * 42.0F, 29U) - 0.5F;
                        const float grain_fine = value_noise_2d(u * 112.0F, v * 112.0F, 53U) - 0.5F;
                        const float grain = grain_coarse * 0.65F + grain_fine * 0.35F;
                        const float nx_g = value_noise_2d((u + 0.004F) * 84.0F, v * 84.0F, 61U) -
                                           value_noise_2d((u - 0.004F) * 84.0F, v * 84.0F, 61U);
                        const float ny_g = value_noise_2d(u * 84.0F, (v + 0.004F) * 84.0F, 67U) -
                                           value_noise_2d(u * 84.0F, (v - 0.004F) * 84.0F, 67U);

                        if (sub.paint_target == CharacterPaintTarget::outfit_secondary && v >= 0.80F) {
                            // Footwear zone (0.80..0.935 upper leather, 0.935..1.0 sole tread)
                            if (v >= 0.935F) {
                                col = boot_lin * 0.38F;
                                metallic = 0.02F;
                                roughness = 0.88F;
                            } else {
                                col = boot_lin * (0.95F + 0.07F * grain);
                                metallic = 0.04F;
                                roughness = 0.50F;
                                nrm = glm::normalize(glm::vec3(nx_g * 0.08F, ny_g * 0.08F, 1.0F));
                            }
                        } else if (sub.paint_target == CharacterPaintTarget::outfit_secondary &&
                                   v >= 0.730F && v < 0.794F) {
                            // Waist belt band + metallic buckle on exact front center (u >= 0.462)
                            if (u >= 0.462F) {
                                col = acc_lin;
                                metallic = 0.88F;
                                roughness = 0.28F;
                            } else {
                                col = boot_lin * (0.80F + 0.06F * grain);
                                metallic = 0.05F;
                                roughness = 0.50F;
                            }
                        } else if (sub.paint_target == CharacterPaintTarget::outfit_secondary &&
                                   v >= 0.700F && v < 0.725F && pkg.wardrobe.has_gloves) {
                            // Gloves share dark leather trim blend with secondary garment
                            col = glm::mix(boot_lin, main_lin, 0.30F) * (0.96F + 0.05F * grain);
                            metallic = 0.05F;
                            roughness = 0.52F;
                        } else {
                            col = main_lin * (0.97F + 0.06F * grain);
                            nrm = glm::normalize(glm::vec3(nx_g * 0.10F, ny_g * 0.10F, 1.0F));
                            // Subtle tonal center placket seam inside torso panel (avoids shoulder seam v < 0.10)
                            if (sub.paint_target == CharacterPaintTarget::outfit_primary &&
                                v >= 0.12F && v <= 0.64F && std::abs(u - 0.50F) < 0.005F) {
                                col = main_lin * 0.84F;
                            }
                        }

                        if (pkg.wardrobe.wear_and_tear > 0.02F) {
                            const float grit = value_noise_2d(u * 18.0F, v * 18.0F, 77U);
                            if (grit > 1.0F - pkg.wardrobe.wear_and_tear * 0.35F) {
                                col *= 0.84F;
                                roughness = (std::min)(0.95F, roughness + 0.14F);
                            }
                        }
                    }
                }

                // Apply user/AI paint strokes targeting this slot (painted decals, camo, scars, tattoos)
                for (const CharacterPaintStroke& ps : slot_strokes) {
                    const int mirrors = ps.mirror_u ? 2 : 1;
                    for (int m = 0; m < mirrors; ++m) {
                        const float pu = (m == 1) ? (1.0F - ps.uv.x) : ps.uv.x;
                        const float dist = std::hypot(u - pu, v - ps.uv.y);
                        if (dist >= ps.radius) {
                            continue;
                        }
                        const float norm_d = dist / ps.radius;
                        const float edge = 1.0F - std::pow(norm_d, 1.0F + ps.hardness * 3.0F);
                        const float alpha = std::clamp(edge * ps.strength * ps.color.a, 0.0F, 1.0F);
                        const glm::vec3 p_lin(
                            srgb_to_linear_channel(ps.color.r),
                            srgb_to_linear_channel(ps.color.g),
                            srgb_to_linear_channel(ps.color.b));
                        if (ps.mode == CharacterPaintMode::erase) {
                            const glm::vec3 fallback_lin(
                                srgb_to_linear_channel(sub.material.base_color.r),
                                srgb_to_linear_channel(sub.material.base_color.g),
                                srgb_to_linear_channel(sub.material.base_color.b));
                            col = glm::mix(col, fallback_lin, alpha);
                        } else if (ps.mode == CharacterPaintMode::scar) {
                            col = glm::mix(col, glm::vec3(0.45F, 0.16F, 0.14F), alpha * 0.85F);
                            const glm::vec3 delta = glm::normalize(glm::vec3(
                                (u - pu) * 18.0F, (v - ps.uv.y) * 18.0F, 1.0F));
                            nrm = glm::normalize(glm::mix(nrm, delta, alpha));
                            roughness = glm::mix(roughness, 0.36F, alpha);
                        } else if (ps.mode == CharacterPaintMode::metallic_accent) {
                            col = glm::mix(col, p_lin, alpha);
                            metallic = glm::mix(metallic, 0.92F, alpha);
                            roughness = glm::mix(roughness, 0.22F, alpha);
                        } else {
                            col = glm::mix(col, p_lin, alpha);
                        }
                    }
                }

                const std::size_t off = (static_cast<std::size_t>(y) * res + x) * 4U;
                set.base_color_rgba[off + 0U] = float_to_unorm8(linear_to_srgb_channel(col.r));
                set.base_color_rgba[off + 1U] = float_to_unorm8(linear_to_srgb_channel(col.g));
                set.base_color_rgba[off + 2U] = float_to_unorm8(linear_to_srgb_channel(col.b));
                set.base_color_rgba[off + 3U] = 255U;

                const glm::vec3 nn = glm::normalize(nrm);
                set.normal_rgba[off + 0U] = float_to_unorm8(nn.x * 0.5F + 0.5F);
                set.normal_rgba[off + 1U] = float_to_unorm8(nn.y * 0.5F + 0.5F);
                set.normal_rgba[off + 2U] = float_to_unorm8(nn.z * 0.5F + 0.5F);
                set.normal_rgba[off + 3U] = 255U;

                set.metallic_roughness_rgba[off + 0U] = float_to_unorm8(std::clamp(ao, 0.0F, 1.0F));
                set.metallic_roughness_rgba[off + 1U] = float_to_unorm8(std::clamp(roughness, minimum_pbr_roughness, 1.0F));
                set.metallic_roughness_rgba[off + 2U] = float_to_unorm8(std::clamp(metallic, 0.0F, 1.0F));
                set.metallic_roughness_rgba[off + 3U] = 255U;
            }
        }

        outputs.push_back(std::move(set));
    }

    return outputs;
}

// ============================================================================
// Procedural Clips & 12-Bone Facial Pose Composer
// ============================================================================

namespace {

[[nodiscard]] float smoothstep01(const float x) noexcept {
    const float t = std::clamp(x, 0.0F, 1.0F);
    return t * t * (3.0F - 2.0F * t);
}

[[nodiscard]] glm::vec3 safe_normalize(
    const glm::vec3& v,
    const glm::vec3& fallback) noexcept {
    const float len2 = glm::dot(v, v);
    if (len2 > 1.0e-10F) {
        return v * (1.0F / std::sqrt(len2));
    }
    return fallback;
}

[[nodiscard]] glm::vec3 proj_perp(
    const glm::vec3& v,
    const glm::vec3& axis_unit) noexcept {
    return v - axis_unit * glm::dot(v, axis_unit);
}

[[nodiscard]] glm::mat3 make_ortho_frame_01(
    const glm::vec3& x_dir,
    const glm::vec3& y_hint,
    const glm::vec3& fallback_y) noexcept {
    const glm::vec3 x = safe_normalize(x_dir, glm::vec3(0.0F, 1.0F, 0.0F));
    glm::vec3 y_p = proj_perp(y_hint, x);
    if (glm::dot(y_p, y_p) < 1.0e-8F) {
        y_p = proj_perp(fallback_y, x);
        if (glm::dot(y_p, y_p) < 1.0e-8F) {
            const glm::vec3 alt = std::abs(x.z) < 0.9F
                ? glm::vec3(0.0F, 0.0F, 1.0F)
                : glm::vec3(1.0F, 0.0F, 0.0F);
            y_p = proj_perp(alt, x);
        }
    }
    const glm::vec3 y = safe_normalize(y_p, glm::vec3(1.0F, 0.0F, 0.0F));
    const glm::vec3 z = glm::cross(x, y);
    return glm::mat3(x, y, z);
}

[[nodiscard]] glm::quat shortest_arc_quat(
    const glm::vec3& from_unit,
    const glm::vec3& to_unit) noexcept {
    const float d = std::clamp(glm::dot(from_unit, to_unit), -1.0F, 1.0F);
    if (d > 0.99999F) {
        return glm::quat(1.0F, 0.0F, 0.0F, 0.0F);
    }
    if (d < -0.99999F) {
        glm::vec3 ortho = glm::cross(glm::vec3(1.0F, 0.0F, 0.0F), from_unit);
        if (glm::dot(ortho, ortho) < 1.0e-6F) {
            ortho = glm::cross(glm::vec3(0.0F, 1.0F, 0.0F), from_unit);
        }
        return glm::angleAxis(
            glm::pi<float>(),
            safe_normalize(ortho, glm::vec3(0.0F, 0.0F, 1.0F)));
    }
    const glm::vec3 c = glm::cross(from_unit, to_unit);
    return glm::normalize(glm::quat(1.0F + d, c.x, c.y, c.z));
}

[[nodiscard]] float quat_axis_twist_angle(
    const glm::quat& q_in,
    const glm::vec3& axis_unit) noexcept {
    const glm::quat q = (q_in.w < 0.0F) ? -q_in : q_in;
    const float d = glm::dot(glm::vec3(q.x, q.y, q.z), axis_unit);
    const glm::vec3 p = d * axis_unit;
    const float plen = glm::length(p);
    if (plen < 1.0e-7F) {
        return 0.0F;
    }
    const float angle = 2.0F * std::atan2(plen, q.w);
    return d >= 0.0F ? angle : -angle;
}

[[nodiscard]] glm::quat euler_yxz_deg(const glm::vec3& deg) noexcept {
    const glm::vec3 r = glm::radians(deg);
    return glm::angleAxis(r.y, glm::vec3(0.0F, 1.0F, 0.0F)) *
           glm::angleAxis(r.x, glm::vec3(1.0F, 0.0F, 0.0F)) *
           glm::angleAxis(r.z, glm::vec3(0.0F, 0.0F, 1.0F));
}

[[nodiscard]] float pchip_interior_slope(
    const float h0,
    const float h1,
    const float dy0,
    const float dy1) noexcept {
    const float s0 = h0 > 1.0e-6F ? (dy0 / h0) : 0.0F;
    const float s1 = h1 > 1.0e-6F ? (dy1 / h1) : 0.0F;
    if (s0 * s1 <= 0.0F) {
        return 0.0F;
    }
    const float w1 = 2.0F * h1 + h0;
    const float w2 = h1 + 2.0F * h0;
    return (w1 + w2) / (w1 / s0 + w2 / s1);
}

[[nodiscard]] float pchip_periodic_slope(
    const std::span<const std::pair<float, float>> knots,
    const std::size_t idx) noexcept {
    const std::size_t segs = knots.size() - 1U;
    const std::size_t k = idx % segs;
    const std::size_t kp = (k == 0U) ? (segs - 1U) : (k - 1U);
    const float h0 = knots[kp + 1U].first - knots[kp].first;
    const float h1 = knots[k + 1U].first - knots[k].first;
    const float dy0 = knots[kp + 1U].second - knots[kp].second;
    const float dy1 = knots[k + 1U].second - knots[k].second;
    return pchip_interior_slope(h0, h1, dy0, dy1);
}

[[nodiscard]] float pchip_endpoint_slope(
    const float h0,
    const float h1,
    const float s0,
    const float s1) noexcept {
    const float denom = h0 + h1;
    if (denom <= 1.0e-6F) {
        return s0;
    }
    const float d = ((2.0F * h0 + h1) * s0 - h0 * s1) / denom;
    if (d * s0 <= 0.0F) {
        return 0.0F;
    }
    if (std::abs(d) > 3.0F * std::abs(s0)) {
        return 3.0F * s0;
    }
    return d;
}

[[nodiscard]] float eval_pchip_periodic(
    const std::span<const std::pair<float, float>> knots,
    const float u_raw) noexcept {
    if (knots.size() < 3U) {
        return knots.empty() ? 0.0F : knots.front().second;
    }
    const float u = u_raw - std::floor(u_raw);
    const std::size_t segs = knots.size() - 1U;
    std::size_t seg = 0U;
    for (std::size_t i = 0U; i < segs; ++i) {
        if (u <= knots[i + 1U].first || i + 1U == segs) {
            seg = i;
            break;
        }
    }
    const float t0 = knots[seg].first;
    const float t1 = knots[seg + 1U].first;
    const float h = t1 - t0;
    if (h <= 1.0e-6F) {
        return knots[seg].second;
    }
    const float m0 = pchip_periodic_slope(knots, seg);
    const float m1 = pchip_periodic_slope(knots, seg + 1U);
    const float s = std::clamp((u - t0) / h, 0.0F, 1.0F);
    const float s2 = s * s;
    const float s3 = s2 * s;
    return (2.0F * s3 - 3.0F * s2 + 1.0F) * knots[seg].second +
           (s3 - 2.0F * s2 + s) * (h * m0) +
           (-2.0F * s3 + 3.0F * s2) * knots[seg + 1U].second +
           (s3 - s2) * (h * m1);
}

[[nodiscard]] float eval_pchip_clamped(
    const std::span<const std::pair<float, float>> knots,
    const float u_raw) noexcept {
    if (knots.size() < 2U) {
        return knots.empty() ? 0.0F : knots.front().second;
    }
    if (knots.size() == 2U) {
        const float h = knots[1U].first - knots[0U].first;
        const float s = h > 1.0e-6F ? std::clamp((u_raw - knots[0U].first) / h, 0.0F, 1.0F) : 0.0F;
        return glm::mix(knots[0U].second, knots[1U].second, s);
    }
    const float u = std::clamp(u_raw, knots.front().first, knots.back().first);
    const std::size_t segs = knots.size() - 1U;
    std::size_t seg = 0U;
    for (std::size_t i = 0U; i < segs; ++i) {
        if (u <= knots[i + 1U].first || i + 1U == segs) {
            seg = i;
            break;
        }
    }
    const auto slope_at = [&](const std::size_t k) noexcept -> float {
        if (k == 0U) {
            const float h0 = knots[1U].first - knots[0U].first;
            const float h1 = knots[2U].first - knots[1U].first;
            const float s0 = h0 > 1.0e-6F ? (knots[1U].second - knots[0U].second) / h0 : 0.0F;
            const float s1 = h1 > 1.0e-6F ? (knots[2U].second - knots[1U].second) / h1 : 0.0F;
            return pchip_endpoint_slope(h0, h1, s0, s1);
        }
        if (k == segs) {
            const float h0 = knots[segs].first - knots[segs - 1U].first;
            const float h1 = knots[segs - 1U].first - knots[segs - 2U].first;
            const float s0 = h0 > 1.0e-6F ? (knots[segs].second - knots[segs - 1U].second) / h0 : 0.0F;
            const float s1 = h1 > 1.0e-6F ? (knots[segs - 1U].second - knots[segs - 2U].second) / h1 : 0.0F;
            return pchip_endpoint_slope(h0, h1, s0, s1);
        }
        return pchip_interior_slope(
            knots[k].first - knots[k - 1U].first,
            knots[k + 1U].first - knots[k].first,
            knots[k].second - knots[k - 1U].second,
            knots[k + 1U].second - knots[k].second);
    };
    const float t0 = knots[seg].first;
    const float t1 = knots[seg + 1U].first;
    const float h = t1 - t0;
    if (h <= 1.0e-6F) {
        return knots[seg].second;
    }
    const float m0 = slope_at(seg);
    const float m1 = slope_at(seg + 1U);
    const float s = std::clamp((u - t0) / h, 0.0F, 1.0F);
    const float s2 = s * s;
    const float s3 = s2 * s;
    return (2.0F * s3 - 3.0F * s2 + 1.0F) * knots[seg].second +
           (s3 - 2.0F * s2 + s) * (h * m0) +
           (-2.0F * s3 + 3.0F * s2) * knots[seg + 1U].second +
           (s3 - s2) * (h * m1);
}

struct LegRigMetrics {
    std::size_t hip_idx = 0U;
    std::size_t shin_idx = 0U;
    std::size_t foot_idx = 0U;
    std::size_t toe_idx = 0U;
    float side_sign = 1.0F;
    float len_thigh = 0.40F;
    float len_shin = 0.40F;
    glm::vec3 u_thigh0{0.0F, -1.0F, 0.0F};
    glm::vec3 v_shin0{0.0F, -1.0F, 0.0F};
    glm::vec3 n_leg0{1.0F, 0.0F, 0.0F};
    glm::mat3 L_thigh0{1.0F};
    glm::mat3 L_shin0{1.0F};
};

struct ArmRigMetrics {
    std::size_t clavicle_idx = 0U;
    std::size_t upper_idx = 0U;
    std::size_t forearm_idx = 0U;
    std::size_t hand_idx = 0U;
    std::size_t thumb_idx = 0U;
    std::size_t index_idx = 0U;
    std::size_t middle_idx = 0U;
    std::size_t pinky_idx = 0U;
    std::size_t shoulder_twist_idx = 0U;
    std::size_t forearm_twist_idx = 0U;
    std::size_t pectoral_idx = 0U;
    float side_sign = 1.0F;
    glm::vec3 u_arm0{0.0F, -1.0F, 0.0F};
    glm::vec3 v_farm0{0.0F, -1.0F, 0.0F};
    glm::vec3 n_arm0{1.0F, 0.0F, 0.0F};
    glm::vec3 f_hand0{0.0F, -1.0F, 0.0F};
    glm::vec3 p_palm0{-1.0F, 0.0F, 0.0F};
    glm::vec3 c_finger0{0.0F, 0.0F, 1.0F};
    glm::vec3 c_spread0{0.0F, 1.0F, 0.0F};
    glm::vec3 c_thumb0{0.0F, 1.0F, 0.0F};
    glm::mat3 F_upper0{1.0F};
    glm::mat3 F_fore0{1.0F};
    glm::mat3 F_hand0{1.0F};
};

struct HumanRigMetrics {
    float pelvis_y = 0.85F;
    float ankle_bind_y = 0.067F;
    float foot_len = 0.11F;
    float heel_back = 0.05F;
    float stance_half_x = 0.095F;
    float leg_len = 0.80F;
    float leg_ratio = 1.0F;
    LegRigMetrics leg_l{};
    LegRigMetrics leg_r{};
    ArmRigMetrics arm_l{};
    ArmRigMetrics arm_r{};
};

[[nodiscard]] HumanRigMetrics build_human_rig_metrics(
    const CookedSkeletonAsset& skeleton,
    const CharacterBodyParams& body) noexcept {
    std::array<glm::vec3, character_total_bone_count> p{};
    const auto fallback = fallback_character_bone_layout(body);
    for (std::size_t i = 0U; i < character_total_bone_count; ++i) {
        p[i] = fallback[i].model_pos;
    }
    const std::size_t bone_n =
        (std::min)(skeleton.bones.size(), static_cast<std::size_t>(character_total_bone_count));
    for (std::size_t i = 0U; i < bone_n; ++i) {
        const std::int32_t parent = skeleton.bones[i].parent_index;
        if (parent >= 0 && static_cast<std::size_t>(parent) < i) {
            p[i] = p[static_cast<std::size_t>(parent)] + skeleton.bones[i].local_bind_translation;
        } else {
            p[i] = skeleton.bones[i].local_bind_translation;
        }
    }

    const auto extract_leg = [&](
                                 const std::size_t hip,
                                 const std::size_t shin,
                                 const std::size_t foot,
                                 const std::size_t toe,
                                 const float side) noexcept -> LegRigMetrics {
        LegRigMetrics lm{};
        lm.hip_idx = hip;
        lm.shin_idx = shin;
        lm.foot_idx = foot;
        lm.toe_idx = toe;
        lm.side_sign = side;
        lm.len_thigh = (std::max)(glm::distance(p[hip], p[shin]), 0.15F);
        lm.len_shin = (std::max)(glm::distance(p[shin], p[foot]), 0.15F);
        lm.u_thigh0 = safe_normalize(p[shin] - p[hip], glm::vec3(0.0F, -1.0F, 0.0F));
        lm.v_shin0 = safe_normalize(p[foot] - p[shin], glm::vec3(0.0F, -1.0F, 0.0F));
        lm.n_leg0 = safe_normalize(
            glm::cross(glm::vec3(0.0F, 0.0F, 1.0F), lm.u_thigh0),
            glm::vec3(1.0F, 0.0F, 0.0F));
        if (lm.n_leg0.x < 0.0F) {
            lm.n_leg0 = -lm.n_leg0;
        }
        lm.L_thigh0 = make_ortho_frame_01(lm.u_thigh0, lm.n_leg0, glm::vec3(1.0F, 0.0F, 0.0F));
        lm.L_shin0 = make_ortho_frame_01(lm.v_shin0, lm.n_leg0, glm::vec3(1.0F, 0.0F, 0.0F));
        return lm;
    };

    const auto extract_arm = [&](
                                 const std::size_t clav,
                                 const std::size_t upper,
                                 const std::size_t forearm,
                                 const std::size_t hand,
                                 const std::size_t thumb,
                                 const std::size_t index_b,
                                 const std::size_t middle_b,
                                 const std::size_t pinky_b,
                                 const std::size_t shld_tw,
                                 const std::size_t farm_tw,
                                 const std::size_t pec,
                                 const float side) noexcept -> ArmRigMetrics {
        ArmRigMetrics am{};
        am.clavicle_idx = clav;
        am.upper_idx = upper;
        am.forearm_idx = forearm;
        am.hand_idx = hand;
        am.thumb_idx = thumb;
        am.index_idx = index_b;
        am.middle_idx = middle_b;
        am.pinky_idx = pinky_b;
        am.shoulder_twist_idx = shld_tw;
        am.forearm_twist_idx = farm_tw;
        am.pectoral_idx = pec;
        am.side_sign = side;

        am.u_arm0 = safe_normalize(
            p[forearm] - p[upper],
            glm::vec3(side * 0.707F, -0.707F, 0.0F));
        am.v_farm0 = safe_normalize(
            p[hand] - p[forearm],
            glm::vec3(side * 0.50F, -0.55F, 0.65F));

        glm::vec3 hinge = glm::cross(am.u_arm0, am.v_farm0);
        if (glm::dot(hinge, hinge) < 1.0e-5F) {
            hinge = glm::cross(am.u_arm0, glm::vec3(0.0F, 0.35F, 1.0F));
        }
        glm::vec3 hinge_unit = safe_normalize(hinge, glm::vec3(1.0F, 0.0F, 0.0F));
        if (glm::dot(glm::cross(hinge_unit, am.u_arm0), glm::vec3(0.0F, 0.35F, 1.0F)) < 0.0F) {
            hinge_unit = -hinge_unit;
        }
        am.n_arm0 = -hinge_unit;
        am.F_upper0 = make_ortho_frame_01(am.u_arm0, am.n_arm0, glm::vec3(1.0F, 0.0F, 0.0F));
        am.F_fore0 = make_ortho_frame_01(am.v_farm0, am.n_arm0, am.n_arm0);

        am.f_hand0 = safe_normalize(p[middle_b] - p[hand], am.v_farm0);
        const glm::vec3 u_ulnar0 = safe_normalize(
            proj_perp(p[pinky_b] - p[index_b], am.f_hand0),
            glm::vec3(0.0F, 0.0F, -1.0F));
        am.p_palm0 = (side > 0.0F)
            ? safe_normalize(glm::cross(u_ulnar0, am.f_hand0), -am.n_arm0)
            : safe_normalize(glm::cross(am.f_hand0, u_ulnar0), am.n_arm0);
        const glm::vec3 n_hand0 = -side * am.p_palm0;
        am.F_hand0 = make_ortho_frame_01(am.f_hand0, n_hand0, am.n_arm0);
        am.c_finger0 = safe_normalize(glm::cross(am.f_hand0, am.p_palm0), am.n_arm0);
        am.c_spread0 = safe_normalize(glm::cross(am.f_hand0, -u_ulnar0), am.p_palm0);
        const glm::vec3 t_dir0 = safe_normalize(p[thumb] - p[hand], am.f_hand0);
        const glm::vec3 t_target0 = safe_normalize(0.25F * am.p_palm0 + 0.75F * u_ulnar0, am.p_palm0);
        am.c_thumb0 = safe_normalize(glm::cross(t_dir0, t_target0), am.c_finger0);
        return am;
    };

    HumanRigMetrics m{};
    m.pelvis_y = p[1U].y;
    m.ankle_bind_y = 0.5F * (p[25U].y + p[29U].y);
    const float fl_l = p[26U].z - p[25U].z;
    const float fl_r = p[30U].z - p[29U].z;
    m.foot_len = std::clamp(0.5F * (fl_l + fl_r), 0.08F, 0.18F);
    m.heel_back = m.foot_len * 0.45F;
    const float hip_bind_x = 0.5F * (std::abs(p[23U].x) + std::abs(p[27U].x));
    m.stance_half_x = std::clamp(hip_bind_x * 0.96F, 0.085F, 0.125F);
    m.leg_l = extract_leg(23U, 24U, 25U, 26U, 1.0F);
    m.leg_r = extract_leg(27U, 28U, 29U, 30U, -1.0F);
    m.leg_len = 0.5F * ((m.leg_l.len_thigh + m.leg_l.len_shin) +
                        (m.leg_r.len_thigh + m.leg_r.len_shin));
    m.leg_ratio = std::clamp(m.leg_len / 0.80F, 0.65F, 1.50F);
    m.arm_l = extract_arm(7U, 8U, 9U, 10U, 11U, 12U, 13U, 14U, 31U, 32U, 35U, 1.0F);
    m.arm_r = extract_arm(15U, 16U, 17U, 18U, 19U, 20U, 21U, 22U, 33U, 34U, 36U, -1.0F);
    return m;
}

struct FootTargetSpec {
    float x = 0.10F;
    float lift_y = 0.0F;
    float z = 0.0F;
    float yaw_deg = 0.0F;
    float rocker_deg = 0.0F;
    float knee_out_bias = 0.06F;
};

struct ArmTargetSpec {
    float elevation_deg = 14.5F;
    float azimuth_deg = 10.0F;
    float elbow_flex_deg = 16.0F;
    float pronation_deg = 10.0F;
    float wrist_pitch_deg = 0.0F;
    float wrist_ulnar_deg = 0.0F;
    float finger_curl = 0.30F;
    float finger_spread = 0.25F;
    glm::vec3 elbow_pole_local{0.22F, -0.12F, -0.96F};
};

struct HumanPoseSpec {
    glm::vec3 pelvis_offset{0.0F};
    glm::vec3 pelvis_euler_deg{0.0F};
    glm::vec3 spine_euler_deg{0.0F};
    glm::vec3 head_euler_deg{0.0F};
    float tailcoat_pitch_deg = 0.0F;
    float breath = 0.0F;
    FootTargetSpec foot_l{};
    FootTargetSpec foot_r{};
    ArmTargetSpec arm_l{};
    ArmTargetSpec arm_r{};
    float blink = 0.0F;
    float smile = 0.0F;
    float jaw_open = 0.0F;
    float lip_upper_raise = 0.0F;
    float lip_round = 0.0F;
    float brow_lift = 0.0F;
};

void solve_human_pose(
    const CookedSkeletonAsset& skeleton,
    const HumanRigMetrics& metrics,
    const HumanPoseSpec& spec,
    SkeletonPose& out_pose) noexcept {
    set_bind_pose(skeleton, out_pose);
    if (skeleton.bones.size() < character_total_bone_count) {
        return;
    }

    out_pose.local[1U].translation =
        skeleton.bones[1U].local_bind_translation + spec.pelvis_offset;
    out_pose.local[1U].rotation = euler_yxz_deg(spec.pelvis_euler_deg);
    out_pose.local[2U].rotation = euler_yxz_deg(spec.spine_euler_deg * 0.35F);
    out_pose.local[3U].rotation = euler_yxz_deg(
        spec.spine_euler_deg * 0.35F + glm::vec3(-spec.breath * 1.4F, 0.0F, 0.0F));
    out_pose.local[4U].rotation = euler_yxz_deg(
        spec.spine_euler_deg * 0.30F + glm::vec3(-spec.breath * 0.8F, 0.0F, 0.0F));
    out_pose.local[5U].rotation = euler_yxz_deg(spec.head_euler_deg * 0.45F);
    out_pose.local[6U].rotation = euler_yxz_deg(spec.head_euler_deg * 0.55F);
    out_pose.local[37U].rotation = euler_yxz_deg({spec.tailcoat_pitch_deg, 0.0F, 0.0F});

    const auto apply_clavicle = [&](const ArmRigMetrics& am, const ArmTargetSpec& at) noexcept {
        const float clav_elev_deg = std::clamp(
            0.22F * (std::max)(0.0F, at.elevation_deg - 22.0F) + spec.breath * 1.4F,
            -6.0F,
            24.0F);
        const float clav_protract_deg = std::clamp(
            0.14F * at.elevation_deg * std::sin(glm::radians(at.azimuth_deg)),
            -10.0F,
            12.0F);
        out_pose.local[am.clavicle_idx].rotation = euler_yxz_deg(
            {0.0F, -am.side_sign * clav_protract_deg, am.side_sign * clav_elev_deg});
        out_pose.local[am.pectoral_idx].rotation = euler_yxz_deg(
            {0.0F, -am.side_sign * 0.15F * clav_protract_deg, am.side_sign * 0.15F * clav_elev_deg});
    };
    apply_clavicle(metrics.arm_l, spec.arm_l);
    apply_clavicle(metrics.arm_r, spec.arm_r);

    compute_model_space_pose(skeleton, out_pose);

    const glm::quat r_pelvis_model =
        glm::normalize(glm::quat_cast(glm::mat3(out_pose.model_space[1U])));
    const auto solve_leg = [&](const LegRigMetrics& lm, const FootTargetSpec& ft) noexcept {
        const glm::quat q_foot_yaw = glm::angleAxis(
            glm::radians(ft.yaw_deg),
            glm::vec3(0.0F, 1.0F, 0.0F));
        const float rocker_rad = glm::radians(ft.rocker_deg);
        const float c_r = std::cos(rocker_rad);
        const float s_r = std::sin(rocker_rad);
        glm::vec3 ankle_local_offset(0.0F, metrics.ankle_bind_y, 0.0F);
        float toe_local_pitch_deg = 0.0F;
        if (ft.rocker_deg < -0.05F) {
            const glm::vec3 v_heel(0.0F, -metrics.ankle_bind_y, -metrics.heel_back);
            const glm::vec3 v_rot(0.0F, v_heel.y * c_r - v_heel.z * s_r, v_heel.y * s_r + v_heel.z * c_r);
            ankle_local_offset += (v_heel - v_rot);
            toe_local_pitch_deg = std::clamp(0.20F * ft.rocker_deg, -8.0F, 0.0F);
        } else if (ft.rocker_deg > 0.05F) {
            const glm::vec3 v_ball(0.0F, -metrics.ankle_bind_y, metrics.foot_len);
            const glm::vec3 v_rot(0.0F, v_ball.y * c_r - v_ball.z * s_r, v_ball.y * s_r + v_ball.z * c_r);
            ankle_local_offset += (v_ball - v_rot);
            const float ground_blend = 1.0F - smoothstep01((ft.lift_y - 0.015F) / 0.045F);
            toe_local_pitch_deg =
                -0.85F * ft.rocker_deg * ground_blend +
                0.28F * ft.rocker_deg * (1.0F - ground_blend);
        }

        const glm::vec3 base_foot_pos(lm.side_sign * ft.x, ft.lift_y, ft.z);
        const glm::vec3 p_ankle = base_foot_pos + q_foot_yaw * ankle_local_offset;
        const glm::vec3 p_hip(out_pose.model_space[lm.hip_idx][3U]);
        const glm::vec3 to_ankle = p_ankle - p_hip;

        const float max_reach = 0.996F * (lm.len_thigh + lm.len_shin);
        const float min_reach = std::abs(lm.len_thigh - lm.len_shin) + 0.05F;
        const float dist = std::clamp(glm::length(to_ankle), min_reach, max_reach);
        const glm::vec3 d_hat = safe_normalize(to_ankle, glm::vec3(0.0F, -1.0F, 0.0F));
        const float cos_hip = std::clamp(
            (lm.len_thigh * lm.len_thigh + dist * dist - lm.len_shin * lm.len_shin) /
                (2.0F * lm.len_thigh * dist),
            -1.0F,
            1.0F);
        const float sin_hip = std::sqrt((std::max)(0.0F, 1.0F - cos_hip * cos_hip));

        const glm::quat q_pole_yaw = glm::angleAxis(
            glm::radians(spec.pelvis_euler_deg.y * 0.45F + ft.yaw_deg * 0.55F),
            glm::vec3(0.0F, 1.0F, 0.0F));
        const glm::vec3 knee_pole = safe_normalize(
            q_pole_yaw * glm::vec3(lm.side_sign * ft.knee_out_bias, 0.05F, 1.0F),
            glm::vec3(0.0F, 0.0F, 1.0F));
        const glm::vec3 n1 = safe_normalize(
            glm::cross(knee_pole, d_hat),
            glm::vec3(1.0F, 0.0F, 0.0F));
        const glm::vec3 bend_dir = safe_normalize(glm::cross(d_hat, n1), knee_pole);
        const glm::vec3 p_knee =
            p_hip + (d_hat * cos_hip + bend_dir * sin_hip) * lm.len_thigh;
        const glm::vec3 p_ankle_clamped = p_hip + d_hat * dist;

        const glm::vec3 u1 = safe_normalize(p_knee - p_hip, d_hat);
        const glm::vec3 v1 = safe_normalize(p_ankle_clamped - p_knee, d_hat);
        const glm::mat3 L_thigh1 = make_ortho_frame_01(u1, n1, n1);
        const glm::mat3 L_shin1 = make_ortho_frame_01(v1, n1, n1);

        const glm::quat r_thigh_model =
            glm::normalize(glm::quat_cast(L_thigh1 * glm::transpose(lm.L_thigh0)));
        const glm::quat r_shin_model =
            glm::normalize(glm::quat_cast(L_shin1 * glm::transpose(lm.L_shin0)));
        const glm::quat r_foot_model = glm::normalize(
            q_foot_yaw * glm::angleAxis(rocker_rad, glm::vec3(1.0F, 0.0F, 0.0F)));

        out_pose.local[lm.hip_idx].rotation =
            glm::normalize(glm::conjugate(r_pelvis_model) * r_thigh_model);
        out_pose.local[lm.shin_idx].rotation =
            glm::normalize(glm::conjugate(r_thigh_model) * r_shin_model);
        out_pose.local[lm.foot_idx].rotation =
            glm::normalize(glm::conjugate(r_shin_model) * r_foot_model);
        out_pose.local[lm.toe_idx].rotation =
            glm::angleAxis(glm::radians(toe_local_pitch_deg), glm::vec3(1.0F, 0.0F, 0.0F));
    };
    solve_leg(metrics.leg_l, spec.foot_l);
    solve_leg(metrics.leg_r, spec.foot_r);

    const glm::mat3 r_spine3_mat(out_pose.model_space[4U]);
    const auto solve_arm = [&](const ArmRigMetrics& am, const ArmTargetSpec& at) noexcept {
        const glm::quat r_clav_model =
            glm::normalize(glm::quat_cast(glm::mat3(out_pose.model_space[am.clavicle_idx])));
        const float el_rad = glm::radians(std::clamp(at.elevation_deg, 2.0F, 175.0F));
        const float az_rad = glm::radians(at.azimuth_deg);
        const float sin_el = std::sin(el_rad);
        const glm::vec3 u1_torso(
            am.side_sign * sin_el * std::cos(az_rad),
            -std::cos(el_rad),
            sin_el * std::sin(az_rad));
        const glm::vec3 u1 = safe_normalize(r_spine3_mat * u1_torso, glm::vec3(0.0F, -1.0F, 0.0F));
        const glm::vec3 pole_torso(
            am.side_sign * at.elbow_pole_local.x,
            at.elbow_pole_local.y,
            at.elbow_pole_local.z);
        const glm::vec3 pole = safe_normalize(r_spine3_mat * pole_torso, glm::vec3(0.0F, 0.0F, -1.0F));

        glm::vec3 a1_proj = proj_perp(-pole, u1);
        if (glm::dot(a1_proj, a1_proj) < 1.0e-6F) {
            a1_proj = proj_perp(r_spine3_mat * glm::vec3(0.0F, 0.35F, 1.0F), u1);
        }
        const glm::vec3 a1 = safe_normalize(a1_proj, glm::vec3(0.0F, 0.0F, 1.0F));
        const glm::vec3 n1 = safe_normalize(glm::cross(a1, u1), glm::vec3(1.0F, 0.0F, 0.0F));
        const glm::mat3 F_upper1(u1, n1, glm::cross(u1, n1));
        const glm::quat r_hum_full =
            glm::normalize(glm::quat_cast(F_upper1 * glm::transpose(am.F_upper0)));

        const glm::vec3 u_clav = safe_normalize(r_clav_model * am.u_arm0, u1);
        const glm::quat q_swing = shortest_arc_quat(u_clav, u1);
        const glm::quat r_upper_swing = glm::normalize(q_swing * r_clav_model);
        const glm::quat q_twist_delta = glm::normalize(r_hum_full * glm::conjugate(r_upper_swing));
        const float psi = quat_axis_twist_angle(q_twist_delta, u1);
        const glm::quat r_upper_model =
            glm::normalize(glm::angleAxis(0.50F * psi, u1) * r_upper_swing);
        out_pose.local[am.upper_idx].rotation =
            glm::normalize(glm::conjugate(r_clav_model) * r_upper_model);
        out_pose.local[am.shoulder_twist_idx].rotation =
            glm::angleAxis(0.50F * psi, am.u_arm0);

        const float flex_rad = glm::radians(std::clamp(at.elbow_flex_deg, 2.0F, 145.0F));
        const glm::vec3 v1 = safe_normalize(
            std::cos(flex_rad) * u1 + std::sin(flex_rad) * a1,
            u1);
        const glm::mat3 F_fore1 = make_ortho_frame_01(v1, n1, n1);
        const glm::quat r_fore_model =
            glm::normalize(glm::quat_cast(F_fore1 * glm::transpose(am.F_fore0)));
        out_pose.local[am.forearm_idx].rotation =
            glm::normalize(glm::conjugate(r_upper_model) * r_fore_model);

        const glm::quat q_pron = glm::angleAxis(
            am.side_sign * glm::radians(at.pronation_deg),
            v1);
        const glm::mat3 F_wrist_base = glm::mat3_cast(q_pron) * F_fore1;
        const glm::vec3 n_w = F_wrist_base[1U];
        const glm::vec3 a_w = F_wrist_base[2U];
        const glm::quat q_wrist =
            glm::angleAxis(am.side_sign * glm::radians(at.wrist_pitch_deg), a_w) *
            glm::angleAxis(-glm::radians(at.wrist_ulnar_deg), n_w);
        const glm::mat3 F_hand1 = glm::mat3_cast(q_wrist) * F_wrist_base;
        const glm::quat r_hand_model =
            glm::normalize(glm::quat_cast(F_hand1 * glm::transpose(am.F_hand0)));
        const glm::quat q_fore_to_hand =
            glm::normalize(glm::conjugate(r_fore_model) * r_hand_model);
        out_pose.local[am.hand_idx].rotation = q_fore_to_hand;
        const float phi = quat_axis_twist_angle(q_fore_to_hand, am.v_farm0);
        out_pose.local[am.forearm_twist_idx].rotation =
            glm::angleAxis(0.55F * phi, am.v_farm0);

        const float curl = std::clamp(at.finger_curl, 0.0F, 1.0F);
        constexpr float max_curl_deg = 128.0F;
        const float idx_scale = glm::mix(0.82F, 0.96F, curl);
        const float pnk_scale = glm::mix(1.18F, 1.06F, curl);
        const float spread_base = at.finger_spread * (1.0F - 0.85F * curl);
        out_pose.local[am.index_idx].rotation =
            glm::angleAxis(glm::radians(spread_base * 4.5F), am.c_spread0) *
            glm::angleAxis(glm::radians(idx_scale * curl * max_curl_deg), am.c_finger0);
        out_pose.local[am.middle_idx].rotation =
            glm::angleAxis(glm::radians(curl * max_curl_deg), am.c_finger0);
        out_pose.local[am.pinky_idx].rotation =
            glm::angleAxis(glm::radians(-spread_base * 6.0F), am.c_spread0) *
            glm::angleAxis(glm::radians(pnk_scale * curl * max_curl_deg), am.c_finger0);
        out_pose.local[am.thumb_idx].rotation =
            glm::angleAxis(glm::radians(0.40F * curl * 80.0F + 8.0F), am.c_thumb0);
    };
    solve_arm(metrics.arm_l, spec.arm_l);
    solve_arm(metrics.arm_r, spec.arm_r);

    out_pose.local[38U].rotation =
        glm::angleAxis(glm::radians(spec.jaw_open * 13.5F), glm::vec3(1.0F, 0.0F, 0.0F));
    out_pose.local[39U].translation =
        skeleton.bones[39U].local_bind_translation +
        glm::vec3(0.0F, spec.lip_upper_raise * 0.006F, spec.lip_round * 0.008F);
    out_pose.local[40U].translation =
        skeleton.bones[40U].local_bind_translation +
        glm::vec3(0.0F, -spec.jaw_open * 0.0025F, spec.lip_round * 0.008F);
    const float mc_x = spec.smile * 0.004F - spec.lip_round * 0.005F;
    const float mc_y = spec.smile * 0.007F - spec.jaw_open * 0.0035F;
    const float mc_z = -spec.smile * 0.003F;
    out_pose.local[41U].translation =
        skeleton.bones[41U].local_bind_translation + glm::vec3(mc_x, mc_y, mc_z);
    out_pose.local[42U].translation =
        skeleton.bones[42U].local_bind_translation + glm::vec3(-mc_x, mc_y, mc_z);
    out_pose.local[45U].translation =
        skeleton.bones[45U].local_bind_translation +
        glm::vec3(0.0F, -spec.blink * 0.015F, spec.blink * 0.002F);
    out_pose.local[46U].translation =
        skeleton.bones[46U].local_bind_translation +
        glm::vec3(0.0F, -spec.blink * 0.015F, spec.blink * 0.002F);
    out_pose.local[47U].translation =
        skeleton.bones[47U].local_bind_translation + glm::vec3(0.0F, spec.brow_lift * 0.005F, 0.0F);
    out_pose.local[48U].translation =
        skeleton.bones[48U].local_bind_translation + glm::vec3(0.0F, spec.brow_lift * 0.005F, 0.0F);
    out_pose.local[49U].translation =
        skeleton.bones[49U].local_bind_translation + glm::vec3(0.0F, spec.brow_lift * 0.004F, 0.0F);
}

template <typename EvalFn>
[[nodiscard]] AnimationClip bake_human_clip(
    const std::string_view name,
    const float duration_seconds,
    const bool looping,
    const CookedSkeletonAsset& skeleton,
    const HumanRigMetrics& metrics,
    const bool include_blink,
    const bool include_smile,
    const bool include_speech,
    const bool include_brows,
    EvalFn&& eval_fn) {
    AnimationClip clip{};
    clip.name = std::string(name);
    clip.duration_seconds = duration_seconds;
    clip.ticks_per_second = 30.0F;
    clip.looping = looping;

    const std::size_t bone_n =
        (std::min)(skeleton.bones.size(), static_cast<std::size_t>(character_total_bone_count));
    const std::uint32_t sample_intervals = (std::max)(
        2U,
        static_cast<std::uint32_t>(std::lround(duration_seconds * 30.0F)));
    const std::size_t sample_count = static_cast<std::size_t>(sample_intervals) + 1U;

    std::array<BoneAnimationTrack, character_total_bone_count> bone_tracks{};
    std::array<bool, character_total_bone_count> track_rot_enabled{};
    std::array<bool, character_total_bone_count> track_trans_enabled{};

    if (bone_n > 1U) {
        track_trans_enabled[1U] = true;
    }
    for (std::size_t b = 1U; b < (std::min)(bone_n, std::size_t{38U}); ++b) {
        track_rot_enabled[b] = true;
    }
    if (include_blink && bone_n > 46U) {
        track_trans_enabled[45U] = true;
        track_trans_enabled[46U] = true;
    }
    if ((include_smile || include_speech) && bone_n > 42U) {
        track_trans_enabled[41U] = true;
        track_trans_enabled[42U] = true;
    }
    if (include_speech && bone_n > 40U) {
        track_rot_enabled[38U] = true;
        track_trans_enabled[39U] = true;
        track_trans_enabled[40U] = true;
    }
    if (include_brows && bone_n > 49U) {
        track_rot_enabled[38U] = true;
        track_trans_enabled[47U] = true;
        track_trans_enabled[48U] = true;
        track_trans_enabled[49U] = true;
    }

    for (std::size_t b = 0U; b < bone_n; ++b) {
        if (!track_rot_enabled[b] && !track_trans_enabled[b]) {
            continue;
        }
        bone_tracks[b].bone_name = skeleton.bones[b].name;
        bone_tracks[b].bone_index = static_cast<std::int32_t>(b);
        if (track_rot_enabled[b]) {
            bone_tracks[b].rotations.reserve(sample_count);
        }
        if (track_trans_enabled[b]) {
            bone_tracks[b].translations.reserve(sample_count);
        }
    }

    SkeletonPose pose{};
    for (std::size_t k = 0U; k < sample_count; ++k) {
        const float u = static_cast<float>(k) / static_cast<float>(sample_intervals);
        const float t = (k + 1U == sample_count) ? duration_seconds : duration_seconds * u;

        if (looping && k + 1U == sample_count) {
            for (std::size_t b = 0U; b < bone_n; ++b) {
                if (track_rot_enabled[b]) {
                    bone_tracks[b].rotations.push_back(QuatKey{
                        .time = duration_seconds,
                        .value = bone_tracks[b].rotations.front().value,
                    });
                }
                if (track_trans_enabled[b]) {
                    bone_tracks[b].translations.push_back(Vec3Key{
                        .time = duration_seconds,
                        .value = bone_tracks[b].translations.front().value,
                    });
                }
            }
            continue;
        }

        const HumanPoseSpec spec = eval_fn(t, u);
        solve_human_pose(skeleton, metrics, spec, pose);
        for (std::size_t b = 0U; b < bone_n; ++b) {
            if (track_rot_enabled[b]) {
                glm::quat q = glm::normalize(pose.local[b].rotation);
                if (!bone_tracks[b].rotations.empty() &&
                    glm::dot(q, bone_tracks[b].rotations.back().value) < 0.0F) {
                    q = -q;
                }
                bone_tracks[b].rotations.push_back(QuatKey{.time = t, .value = q});
            }
            if (track_trans_enabled[b]) {
                bone_tracks[b].translations.push_back(Vec3Key{
                    .time = t,
                    .value = pose.local[b].translation,
                });
            }
        }
    }

    clip.tracks.reserve(character_total_bone_count);
    for (std::size_t b = 0U; b < bone_n; ++b) {
        if (track_rot_enabled[b] || track_trans_enabled[b]) {
            clip.tracks.push_back(std::move(bone_tracks[b]));
        }
    }
    bind_clip_to_skeleton(clip, skeleton);
    return clip;
}

} // namespace

std::vector<AnimationClip> generate_character_animation_clips(
    const CharacterPackage& input_pkg,
    const CookedSkeletonAsset& skeleton) {
    CharacterPackage pkg = input_pkg;
    sanitize_character_package(pkg);
    const HumanRigMetrics metrics = build_human_rig_metrics(skeleton, pkg.body);
    const float br = std::clamp(pkg.acting.idle_breath_amplitude, 0.25F, 2.0F);
    const float en = std::clamp(pkg.acting.stride_energy, 0.50F, 1.50F);
    const float sw = std::clamp(pkg.acting.arm_swing, 0.35F, 1.65F);
    constexpr float two_pi = glm::two_pi<float>();

    std::vector<AnimationClip> clips;
    clips.reserve(12U);

    // 1. idle (contrapposto weight shift, thoracic breathing, relaxed arms & progressive fingers, blink)
    {
        auto c = bake_human_clip(
            "idle", 3.60F, true, skeleton, metrics, true, false, false, false,
            [&](const float /*t*/, const float u) noexcept {
                const float w = two_pi * u;
                const float breath_phase = 0.5F - 0.5F * std::cos(w);
                const float sway = std::sin(w);
                const float blink_env = smoothstep01((u - 0.72F) / 0.028F) *
                    (1.0F - smoothstep01((u - 0.76F) / 0.042F));

                HumanPoseSpec s{};
                s.breath = br * breath_phase;
                s.pelvis_offset = {
                    (-0.016F + 0.004F * sway) * metrics.leg_ratio,
                    (-0.015F + 0.002F * breath_phase) * metrics.leg_ratio,
                    0.0F};
                s.pelvis_euler_deg = {1.5F, -2.5F + 1.0F * sway, -2.2F + 0.4F * sway};
                s.spine_euler_deg = {-1.0F, 2.5F - 1.0F * sway, 2.0F - 0.3F * sway};
                s.head_euler_deg = {-0.5F - 0.8F * s.breath, -0.4F * sway, 0.2F};
                s.tailcoat_pitch_deg = 2.0F + 1.0F * breath_phase;
                s.foot_l = {metrics.stance_half_x * 1.12F, 0.0F, 0.045F * metrics.leg_len, 5.0F, 0.0F, 0.07F};
                s.foot_r = {metrics.stance_half_x * 1.04F, 0.0F, -0.020F * metrics.leg_len, -5.0F, 0.0F, 0.06F};
                s.arm_l = {7.8F + 0.4F * s.breath, 16.0F + 2.0F * sway, 16.0F + 1.2F * breath_phase, 10.0F, 2.0F, 0.0F, 0.19F, 0.25F, {0.30F, -0.10F, -0.95F}};
                s.arm_r = {7.8F + 0.4F * s.breath, 16.0F - 2.0F * sway, 17.0F + 1.2F * breath_phase, 10.0F, 2.0F, 0.0F, 0.20F, 0.25F, {0.30F, -0.10F, -0.95F}};
                s.blink = blink_env;
                return s;
            });
        clips.push_back(std::move(c));
    }

    // 2. walk (grounded 3-rocker feet, Law-of-Cosines swing flexion, Pelvis/Spine counter-torsion)
    {
        static constexpr std::array<std::pair<float, float>, 8U> walk_z_knots{{
            {0.00F, 1.00F}, {0.12F, 0.72F}, {0.30F, 0.00F}, {0.45F, -0.62F},
            {0.56F, -1.00F}, {0.72F, -0.15F}, {0.88F, 0.78F}, {1.00F, 1.00F},
        }};
        static constexpr std::array<std::pair<float, float>, 9U> walk_lift_knots{{
            {0.00F, 0.000F}, {0.12F, 0.000F}, {0.30F, 0.000F}, {0.45F, 0.000F},
            {0.55F, 0.014F}, {0.70F, 0.115F}, {0.86F, 0.052F}, {0.97F, 0.003F}, {1.00F, 0.000F},
        }};
        static constexpr std::array<std::pair<float, float>, 9U> walk_rocker_knots{{
            {0.00F, -19.0F}, {0.09F, -2.0F}, {0.13F, 0.0F}, {0.38F, 0.0F},
            {0.47F, 16.0F}, {0.57F, 35.0F}, {0.70F, -4.0F}, {0.88F, -14.0F}, {1.00F, -19.0F},
        }};
        static constexpr std::array<std::pair<float, float>, 6U> walk_pelvis_y_knots{{
            {0.00F, -0.032F}, {0.20F, -0.046F}, {0.56F, -0.010F}, {0.78F, -0.016F},
            {0.92F, -0.026F}, {1.00F, -0.032F},
        }};

        const float stride = 0.34F * metrics.leg_len * en;
        const float step_x = metrics.stance_half_x * 0.94F;
        auto c = bake_human_clip(
            "walk", 1.00F, true, skeleton, metrics, false, false, false, false,
            [&](const float /*t*/, const float u) noexcept {
                const float u_r = std::fmod(u + 0.50F, 1.0F);
                const float step_phase = std::fmod(u * 2.0F, 1.0F);
                const float cos_u = std::cos(two_pi * u);
                const float sin_u = std::sin(two_pi * u);
                const float cos_2u = std::cos(2.0F * two_pi * u);

                HumanPoseSpec s{};
                s.breath = 0.25F * br;
                s.pelvis_offset = {
                    0.022F * metrics.leg_ratio * sin_u,
                    eval_pchip_periodic(walk_pelvis_y_knots, step_phase) * metrics.leg_ratio,
                    0.0F};
                s.pelvis_euler_deg = {
                    3.5F + 1.4F * cos_2u,
                    -5.5F * en * cos_u,
                    3.2F * sin_u};
                s.spine_euler_deg = {
                    4.0F + 1.2F * cos_2u,
                    12.0F * en * cos_u,
                    -2.2F * sin_u};
                s.head_euler_deg = {
                    -5.5F - 1.2F * cos_2u,
                    -6.0F * en * cos_u,
                    -0.8F * sin_u};
                s.tailcoat_pitch_deg = 7.0F + 2.5F * cos_2u;

                s.foot_l = {
                    step_x,
                    eval_pchip_periodic(walk_lift_knots, u) * metrics.leg_ratio,
                    eval_pchip_periodic(walk_z_knots, u) * stride,
                    3.5F,
                    eval_pchip_periodic(walk_rocker_knots, u),
                    0.05F};
                s.foot_r = {
                    step_x,
                    eval_pchip_periodic(walk_lift_knots, u_r) * metrics.leg_ratio,
                    eval_pchip_periodic(walk_z_knots, u_r) * stride,
                    -3.5F,
                    eval_pchip_periodic(walk_rocker_knots, u_r),
                    0.05F};

                const float arm_l_phase = -cos_u;
                const float arm_r_phase = cos_u;
                s.arm_l = {
                    15.0F + 13.0F * arm_l_phase * arm_l_phase * sw,
                    arm_l_phase * 78.0F * sw,
                    26.0F + 14.0F * arm_l_phase * sw,
                    10.0F + 6.0F * arm_l_phase,
                    2.0F,
                    0.0F,
                    0.25F,
                    0.20F,
                    {0.20F, -0.12F, -0.97F}};
                s.arm_r = {
                    15.0F + 13.0F * arm_r_phase * arm_r_phase * sw,
                    arm_r_phase * 78.0F * sw,
                    26.0F + 14.0F * arm_r_phase * sw,
                    10.0F + 6.0F * arm_r_phase,
                    2.0F,
                    0.0F,
                    0.25F,
                    0.20F,
                    {0.20F, -0.12F, -0.97F}};
                return s;
            });
        c.events.push_back({0.10F, AnimationEventKind::footstep, "footstep", "left", 1.0F});
        c.events.push_back({0.60F, AnimationEventKind::footstep, "footstep", "right", 1.0F});
        clips.push_back(std::move(c));
    }

    // 3. run (forward athletic lean, brief double-float flight phase, high heel kick, compact arm pump)
    {
        static constexpr std::array<std::pair<float, float>, 8U> run_z_knots{{
            {0.00F, 0.96F}, {0.14F, 0.05F}, {0.29F, -1.00F}, {0.42F, -0.78F},
            {0.62F, -0.25F}, {0.78F, 0.65F}, {0.92F, 1.00F}, {1.00F, 0.96F},
        }};
        static constexpr std::array<std::pair<float, float>, 9U> run_lift_knots{{
            {0.00F, 0.000F}, {0.06F, 0.000F}, {0.22F, 0.000F}, {0.32F, 0.045F},
            {0.46F, 0.210F}, {0.64F, 0.320F}, {0.82F, 0.140F}, {0.95F, 0.018F}, {1.00F, 0.000F},
        }};
        static constexpr std::array<std::pair<float, float>, 9U> run_rocker_knots{{
            {0.00F, -10.0F}, {0.07F, 0.0F}, {0.21F, 0.0F}, {0.30F, 36.0F},
            {0.44F, 22.0F}, {0.64F, -5.0F}, {0.82F, -14.0F}, {0.95F, -12.0F}, {1.00F, -10.0F},
        }};
        static constexpr std::array<std::pair<float, float>, 6U> run_pelvis_y_knots{{
            {0.00F, -0.048F}, {0.26F, -0.076F}, {0.54F, -0.022F}, {0.78F, 0.022F},
            {0.92F, -0.028F}, {1.00F, -0.048F},
        }};

        const float stride = 0.45F * metrics.leg_len * en;
        const float step_x = metrics.stance_half_x * 0.78F;
        auto c = bake_human_clip(
            "run", 0.68F, true, skeleton, metrics, false, false, false, false,
            [&](const float /*t*/, const float u) noexcept {
                const float u_r = std::fmod(u + 0.50F, 1.0F);
                const float step_phase = std::fmod(u * 2.0F, 1.0F);
                const float cos_u = std::cos(two_pi * u);
                const float sin_u = std::sin(two_pi * u);
                const float cos_2u = std::cos(2.0F * two_pi * u);

                HumanPoseSpec s{};
                s.breath = 0.40F * br;
                s.pelvis_offset = {
                    0.015F * metrics.leg_ratio * sin_u,
                    eval_pchip_periodic(run_pelvis_y_knots, step_phase) * metrics.leg_ratio,
                    0.0F};
                s.pelvis_euler_deg = {
                    7.0F + 2.0F * cos_2u,
                    -7.5F * en * cos_u,
                    3.5F * sin_u};
                s.spine_euler_deg = {
                    14.5F + 1.8F * cos_2u,
                    16.5F * en * cos_u,
                    -2.5F * sin_u};
                s.head_euler_deg = {
                    -16.5F - 2.0F * cos_2u,
                    -8.0F * en * cos_u,
                    -1.0F * sin_u};
                s.tailcoat_pitch_deg = 22.0F + 4.0F * cos_2u;

                s.foot_l = {
                    step_x,
                    eval_pchip_periodic(run_lift_knots, u) * metrics.leg_ratio,
                    eval_pchip_periodic(run_z_knots, u) * stride,
                    2.0F,
                    eval_pchip_periodic(run_rocker_knots, u),
                    0.05F};
                s.foot_r = {
                    step_x,
                    eval_pchip_periodic(run_lift_knots, u_r) * metrics.leg_ratio,
                    eval_pchip_periodic(run_z_knots, u_r) * stride,
                    -2.0F,
                    eval_pchip_periodic(run_rocker_knots, u_r),
                    0.05F};

                const float arm_l_phase = -cos_u;
                const float arm_r_phase = cos_u;
                s.arm_l = {
                    24.0F + 22.0F * arm_l_phase * arm_l_phase * sw,
                    arm_l_phase * (arm_l_phase >= 0.0F ? 102.0F : 74.0F) * sw,
                    86.0F + 14.0F * arm_l_phase,
                    16.0F,
                    4.0F,
                    0.0F,
                    0.76F,
                    0.10F,
                    {0.25F, -0.14F, -0.96F}};
                s.arm_r = {
                    24.0F + 22.0F * arm_r_phase * arm_r_phase * sw,
                    arm_r_phase * (arm_r_phase >= 0.0F ? 102.0F : 74.0F) * sw,
                    86.0F + 14.0F * arm_r_phase,
                    16.0F,
                    4.0F,
                    0.0F,
                    0.76F,
                    0.10F,
                    {0.25F, -0.14F, -0.96F}};
                return s;
            });
        c.events.push_back({0.06F, AnimationEventKind::footstep, "footstep", "left", 1.25F});
        c.events.push_back({0.40F, AnimationEventKind::footstep, "footstep", "right", 1.25F});
        clips.push_back(std::move(c));
    }

    // 4. stop (braking forward plant -> shock absorb -> upright settle)
    {
        static constexpr std::array<std::pair<float, float>, 4U> stop_pelvis_y{{
            {0.00F, -0.058F}, {0.35F, -0.072F}, {0.72F, -0.025F}, {1.00F, -0.015F},
        }};
        static constexpr std::array<std::pair<float, float>, 4U> stop_foot_lz{{
            {0.00F, 0.25F}, {0.35F, 0.20F}, {0.78F, 0.06F}, {1.00F, 0.04F},
        }};
        static constexpr std::array<std::pair<float, float>, 4U> stop_foot_rz{{
            {0.00F, -0.18F}, {0.30F, -0.08F}, {0.70F, 0.00F}, {1.00F, -0.02F},
        }};
        auto c = bake_human_clip(
            "stop", 0.40F, false, skeleton, metrics, false, false, false, false,
            [&](const float /*t*/, const float u) noexcept {
                const float ease = smoothstep01(u);
                const float r_lift = std::sin(glm::pi<float>() * std::clamp((u - 0.15F) / 0.65F, 0.0F, 1.0F));
                HumanPoseSpec s{};
                s.pelvis_offset = {
                    -0.010F * ease * metrics.leg_ratio,
                    eval_pchip_clamped(stop_pelvis_y, u) * metrics.leg_ratio,
                    0.0F};
                s.pelvis_euler_deg = {glm::mix(6.0F, 1.5F, ease), glm::mix(-4.0F, -2.0F, ease), -1.5F * ease};
                s.spine_euler_deg = {glm::mix(-9.0F, -0.8F, ease), glm::mix(6.0F, 2.0F, ease), 1.5F * ease};
                s.head_euler_deg = {glm::mix(4.0F, -0.5F, ease), glm::mix(-2.0F, 0.0F, ease), 0.0F};
                s.tailcoat_pitch_deg = glm::mix(14.0F, 2.0F, ease);
                s.foot_l = {
                    metrics.stance_half_x * 1.12F,
                    0.0F,
                    eval_pchip_clamped(stop_foot_lz, u) * metrics.leg_len,
                    5.0F,
                    glm::mix(-12.0F, 0.0F, smoothstep01(u / 0.35F)),
                    0.07F};
                s.foot_r = {
                    metrics.stance_half_x * 1.06F,
                    0.045F * r_lift * metrics.leg_ratio,
                    eval_pchip_clamped(stop_foot_rz, u) * metrics.leg_len,
                    -5.0F,
                    glm::mix(18.0F, 0.0F, ease),
                    0.06F};
                s.arm_l = {glm::mix(22.0F, 14.5F, ease), glm::mix(-48.0F, 10.0F, ease), glm::mix(34.0F, 16.0F, ease), 12.0F, 2.0F, 0.0F, 0.25F, 0.25F, {0.22F, -0.12F, -0.96F}};
                s.arm_r = {glm::mix(24.0F, 14.5F, ease), glm::mix(52.0F, 10.0F, ease), glm::mix(44.0F, 17.0F, ease), 12.0F, 2.0F, 0.0F, 0.25F, 0.25F, {0.22F, -0.12F, -0.96F}};
                return s;
            });
        c.events.push_back({0.08F, AnimationEventKind::footstep, "footstep", "left", 1.05F});
        c.events.push_back({0.30F, AnimationEventKind::footstep, "footstep", "right", 0.75F});
        clips.push_back(std::move(c));
    }

    // 5 & 6. turn_left / turn_right (athletic pivot-in-place turn step cadence)
    for (const bool is_left : {true, false}) {
        const float dir = is_left ? 1.0F : -1.0F;
        auto c = bake_human_clip(
            is_left ? "turn_left" : "turn_right",
            0.60F, true, skeleton, metrics, false, false, false, false,
            [&](const float /*t*/, const float u) noexcept {
                const float w = two_pi * u;
                const float sin_w = std::sin(w);
                const float cos_w = std::cos(w);
                const float l_step = is_left ? smoothstep01(std::sin(glm::pi<float>() * std::clamp(u * 2.0F, 0.0F, 1.0F)))
                                             : smoothstep01(std::sin(glm::pi<float>() * std::clamp((u - 0.50F) * 2.0F, 0.0F, 1.0F)));
                const float r_step = is_left ? smoothstep01(std::sin(glm::pi<float>() * std::clamp((u - 0.50F) * 2.0F, 0.0F, 1.0F)))
                                             : smoothstep01(std::sin(glm::pi<float>() * std::clamp(u * 2.0F, 0.0F, 1.0F)));

                HumanPoseSpec s{};
                s.pelvis_offset = {
                    dir * -0.018F * sin_w * metrics.leg_ratio,
                    (-0.022F - 0.008F * std::cos(2.0F * w)) * metrics.leg_ratio,
                    0.0F};
                s.pelvis_euler_deg = {2.5F, dir * (5.0F + 6.5F * sin_w), dir * 2.0F * sin_w};
                s.spine_euler_deg = {3.0F, dir * (7.0F + 5.0F * cos_w), dir * -1.5F * sin_w};
                s.head_euler_deg = {-3.0F, dir * 8.0F, 0.0F};
                s.tailcoat_pitch_deg = 4.0F;
                s.foot_l = {
                    metrics.stance_half_x * 1.15F,
                    0.055F * l_step * metrics.leg_ratio,
                    dir * 0.045F * sin_w * metrics.leg_len,
                    4.0F + dir * (6.0F + 8.0F * sin_w),
                    -6.0F * l_step,
                    0.07F};
                s.foot_r = {
                    metrics.stance_half_x * 1.15F,
                    0.055F * r_step * metrics.leg_ratio,
                    -dir * 0.045F * sin_w * metrics.leg_len,
                    -4.0F + dir * (6.0F - 8.0F * cos_w),
                    -6.0F * r_step,
                    0.07F};
                s.arm_l = {19.0F, 22.0F + dir * -14.0F * cos_w, 30.0F + 6.0F * sin_w, 12.0F, 2.0F, 0.0F, 0.25F, 0.25F, {0.24F, -0.14F, -0.96F}};
                s.arm_r = {19.0F, 22.0F + dir * 14.0F * cos_w, 30.0F - 6.0F * sin_w, 12.0F, 2.0F, 0.0F, 0.25F, 0.25F, {0.24F, -0.14F, -0.96F}};
                return s;
            });
        clips.push_back(std::move(c));
    }

    // 7. jump (counter-movement crouch -> explosive triple extension push-off -> aerial split tuck)
    {
        static constexpr std::array<std::pair<float, float>, 5U> jump_pelvis_y{{
            {0.00F, -0.02F}, {0.30F, -0.145F}, {0.56F, 0.020F}, {0.82F, 0.065F}, {1.00F, 0.045F},
        }};
        static constexpr std::array<std::pair<float, float>, 5U> jump_lift_l{{
            {0.00F, 0.00F}, {0.32F, 0.00F}, {0.58F, 0.04F}, {0.82F, 0.23F}, {1.00F, 0.21F},
        }};
        static constexpr std::array<std::pair<float, float>, 5U> jump_lift_r{{
            {0.00F, 0.00F}, {0.32F, 0.00F}, {0.58F, 0.02F}, {0.82F, 0.12F}, {1.00F, 0.14F},
        }};
        static constexpr std::array<std::pair<float, float>, 5U> jump_rocker{{
            {0.00F, 0.0F}, {0.28F, -8.0F}, {0.56F, 34.0F}, {0.84F, 16.0F}, {1.00F, 12.0F},
        }};
        static constexpr std::array<std::pair<float, float>, 4U> jump_arm_el{{
            {0.00F, 18.0F}, {0.28F, 38.0F}, {0.62F, 114.0F}, {1.00F, 88.0F},
        }};
        static constexpr std::array<std::pair<float, float>, 4U> jump_arm_az{{
            {0.00F, 15.0F}, {0.28F, -78.0F}, {0.62F, 68.0F}, {1.00F, 52.0F},
        }};
        auto c = bake_human_clip(
            "jump", 0.55F, false, skeleton, metrics, false, false, false, false,
            [&](const float /*t*/, const float u) noexcept {
                const float drive = smoothstep01((u - 0.25F) / 0.35F);
                const float crouch = std::sin(glm::pi<float>() * std::clamp(u / 0.45F, 0.0F, 1.0F));
                HumanPoseSpec s{};
                s.pelvis_offset = {0.0F, eval_pchip_clamped(jump_pelvis_y, u) * metrics.leg_ratio, 0.0F};
                s.pelvis_euler_deg = {6.0F * crouch - 4.0F * drive, 0.0F, 0.0F};
                s.spine_euler_deg = {15.0F * crouch - 8.0F * drive, 0.0F, 0.0F};
                s.head_euler_deg = {-10.0F * crouch + 4.0F * drive, 0.0F, 0.0F};
                s.tailcoat_pitch_deg = 8.0F + 18.0F * drive;
                s.foot_l = {
                    metrics.stance_half_x * 1.12F,
                    eval_pchip_clamped(jump_lift_l, u) * metrics.leg_ratio,
                    0.12F * drive * metrics.leg_len,
                    4.0F,
                    eval_pchip_clamped(jump_rocker, u),
                    0.07F};
                s.foot_r = {
                    metrics.stance_half_x * 1.12F,
                    eval_pchip_clamped(jump_lift_r, u) * metrics.leg_ratio,
                    -0.08F * drive * metrics.leg_len,
                    -4.0F,
                    eval_pchip_clamped(jump_rocker, u),
                    0.07F};
                const float el = eval_pchip_clamped(jump_arm_el, u);
                const float az = eval_pchip_clamped(jump_arm_az, u);
                const float flex = 28.0F + 24.0F * drive;
                s.arm_l = {el, az, flex, 8.0F, 0.0F, 0.0F, 0.36F, 0.25F, {0.28F, -0.30F, -0.91F}};
                s.arm_r = {el, az, flex, 8.0F, 0.0F, 0.0F, 0.36F, 0.25F, {0.28F, -0.30F, -0.91F}};
                return s;
            });
        clips.push_back(std::move(c));
    }

    // 8. fall (looping aerial fall posture with windmilling balance arms & bent knees)
    {
        auto c = bake_human_clip(
            "fall", 0.80F, true, skeleton, metrics, false, false, false, false,
            [&](const float /*t*/, const float u) noexcept {
                const float w = two_pi * u;
                const float sw_a = std::sin(w);
                const float sw_b = std::cos(w);
                HumanPoseSpec s{};
                s.pelvis_offset = {0.0F, 0.025F * metrics.leg_ratio + 0.015F * sw_a * metrics.leg_ratio, 0.0F};
                s.pelvis_euler_deg = {4.0F + 2.0F * sw_b, 3.0F * sw_a, 0.0F};
                s.spine_euler_deg = {8.0F + 3.0F * sw_a, -3.0F * sw_a, 0.0F};
                s.head_euler_deg = {-10.0F - 2.0F * sw_a, 0.0F, 0.0F};
                s.tailcoat_pitch_deg = 28.0F + 8.0F * sw_a;
                s.foot_l = {
                    metrics.stance_half_x * 1.25F,
                    (0.22F + 0.03F * sw_a) * metrics.leg_ratio,
                    0.12F * metrics.leg_len,
                    8.0F,
                    12.0F + 5.0F * sw_b,
                    0.09F};
                s.foot_r = {
                    metrics.stance_half_x * 1.25F,
                    (0.14F - 0.03F * sw_a) * metrics.leg_ratio,
                    -0.06F * metrics.leg_len,
                    -8.0F,
                    18.0F - 5.0F * sw_b,
                    0.09F};
                s.arm_l = {68.0F + 8.0F * sw_a, 24.0F + 14.0F * sw_b, 50.0F + 8.0F * sw_a, 6.0F, -4.0F, 0.0F, 0.22F, 0.65F, {0.32F, -0.55F, -0.77F}};
                s.arm_r = {68.0F - 8.0F * sw_a, 24.0F - 14.0F * sw_b, 50.0F - 8.0F * sw_a, 6.0F, -4.0F, 0.0F, 0.22F, 0.65F, {0.32F, -0.55F, -0.77F}};
                return s;
            });
        clips.push_back(std::move(c));
    }

    // 9. land (impact absorption squat -> upright recovery)
    {
        static constexpr std::array<std::pair<float, float>, 4U> land_pelvis_y{{
            {0.00F, -0.055F}, {0.28F, -0.185F}, {0.65F, -0.085F}, {1.00F, -0.018F},
        }};
        static constexpr std::array<std::pair<float, float>, 4U> land_spine_p{{
            {0.00F, 12.0F}, {0.28F, 22.0F}, {0.65F, 10.0F}, {1.00F, 0.0F},
        }};
        static constexpr std::array<std::pair<float, float>, 4U> land_rocker{{
            {0.00F, 12.0F}, {0.18F, 0.0F}, {0.65F, 0.0F}, {1.00F, 0.0F},
        }};
        auto c = bake_human_clip(
            "land", 0.35F, false, skeleton, metrics, false, false, false, false,
            [&](const float /*t*/, const float u) noexcept {
                const float ease = smoothstep01(u);
                const float absorb = std::sin(glm::pi<float>() * std::clamp(u / 0.72F, 0.0F, 1.0F));
                HumanPoseSpec s{};
                s.pelvis_offset = {0.0F, eval_pchip_clamped(land_pelvis_y, u) * metrics.leg_ratio, 0.0F};
                s.pelvis_euler_deg = {7.0F * absorb + 1.5F * ease, 0.0F, 0.0F};
                s.spine_euler_deg = {eval_pchip_clamped(land_spine_p, u), 0.0F, 0.0F};
                s.head_euler_deg = {-0.75F * s.spine_euler_deg.x, 0.0F, 0.0F};
                s.tailcoat_pitch_deg = 12.0F * absorb + 2.0F;
                s.foot_l = {metrics.stance_half_x * 1.18F, 0.0F, 0.035F * metrics.leg_len, 6.0F, eval_pchip_clamped(land_rocker, u), 0.08F};
                s.foot_r = {metrics.stance_half_x * 1.18F, 0.0F, -0.025F * metrics.leg_len, -6.0F, eval_pchip_clamped(land_rocker, u), 0.08F};
                s.arm_l = {16.0F + 22.0F * absorb, 12.0F - 46.0F * absorb, 18.0F + 28.0F * absorb, 14.0F, 2.0F, 0.0F, 0.26F, 0.25F, {0.24F, -0.14F, -0.96F}};
                s.arm_r = {16.0F + 22.0F * absorb, 12.0F - 46.0F * absorb, 18.0F + 28.0F * absorb, 14.0F, 2.0F, 0.0F, 0.26F, 0.25F, {0.24F, -0.14F, -0.96F}};
                return s;
            });
        c.events.push_back({0.05F, AnimationEventKind::footstep, "footstep", "both", 1.35F});
        clips.push_back(std::move(c));
    }

    // 10. wave (friendly raised right-arm wave with +Z palm, ShoulderTwist_R/ForearmTwist_R articulation & smile)
    {
        auto c = bake_human_clip(
            "wave", 1.60F, true, skeleton, metrics, true, true, false, false,
            [&](const float /*t*/, const float u) noexcept {
                const float w = two_pi * u;
                const float wave2 = std::sin(2.0F * w);
                const float wave2c = std::cos(2.0F * w);
                const float sway = std::sin(w);
                const float blink_env = smoothstep01((u - 0.80F) / 0.03F) *
                    (1.0F - smoothstep01((u - 0.86F) / 0.04F));

                HumanPoseSpec s{};
                s.breath = 0.45F * br * (0.5F - 0.5F * std::cos(w));
                s.pelvis_offset = {0.018F * metrics.leg_ratio, -0.015F * metrics.leg_ratio, 0.0F};
                s.pelvis_euler_deg = {1.5F, 2.5F + 1.5F * sway, 2.2F};
                s.spine_euler_deg = {-2.5F, -3.5F + 2.5F * wave2, -2.5F};
                s.head_euler_deg = {-1.0F, -2.0F + 1.5F * sway, 3.0F * sway};
                s.tailcoat_pitch_deg = 2.5F;
                s.foot_l = {metrics.stance_half_x * 1.10F, 0.0F, -0.015F * metrics.leg_len, 5.0F, 0.0F, 0.06F};
                s.foot_r = {metrics.stance_half_x * 1.12F, 0.0F, 0.045F * metrics.leg_len, -5.0F, 0.0F, 0.07F};
                s.arm_l = {14.5F, 11.0F, 18.0F, 12.0F, 2.0F, 0.0F, 0.20F, 0.25F, {0.22F, -0.12F, -0.96F}};
                s.arm_r = {
                    76.0F + 3.5F * wave2,
                    28.0F + 5.0F * wave2c,
                    98.0F + 8.0F * wave2,
                    -22.0F + 10.0F * wave2,
                    -8.0F,
                    14.0F * wave2c,
                    0.10F,
                    0.90F,
                    {0.00F, -0.88F, -0.47F}};
                s.smile = 0.72F + 0.16F * sway;
                s.blink = blink_env;
                return s;
            });
        clips.push_back(std::move(c));
    }

    // 11. talk (looping expressive presenter gestures + calibrated TMJ speech cadence)
    {
        static constexpr std::array<std::pair<float, float>, 9U> talk_jaw_knots{{
            {0.00F, 0.04F}, {0.10F, 0.30F}, {0.21F, 0.80F}, {0.34F, 0.18F},
            {0.48F, 0.68F}, {0.62F, 0.12F}, {0.75F, 0.86F}, {0.88F, 0.22F}, {1.00F, 0.04F},
        }};
        auto c = bake_human_clip(
            "talk", 1.40F, true, skeleton, metrics, true, true, true, true,
            [&](const float /*t*/, const float u) noexcept {
                const float w = two_pi * u;
                const float g1 = std::sin(w);
                const float g2 = std::cos(w);
                const float jaw = eval_pchip_periodic(talk_jaw_knots, u);
                const float blink_env = smoothstep01((u - 0.84F) / 0.03F) *
                    (1.0F - smoothstep01((u - 0.90F) / 0.04F));

                HumanPoseSpec s{};
                s.breath = 0.40F * br * (0.5F - 0.5F * g2);
                s.pelvis_offset = {-0.012F * metrics.leg_ratio, -0.015F * metrics.leg_ratio, 0.0F};
                s.pelvis_euler_deg = {1.5F, -2.5F + 2.0F * g1, -1.8F};
                s.spine_euler_deg = {-1.0F, 4.0F + 3.5F * g2, 1.5F};
                s.head_euler_deg = {2.5F * g1, -2.0F + 4.5F * g1, 2.5F * g2};
                s.tailcoat_pitch_deg = 2.0F;
                s.foot_l = {metrics.stance_half_x * 1.12F, 0.0F, 0.04F * metrics.leg_len, 5.0F, 0.0F, 0.07F};
                s.foot_r = {metrics.stance_half_x * 1.08F, 0.0F, -0.015F * metrics.leg_len, -5.0F, 0.0F, 0.06F};
                s.arm_l = {
                    38.0F + 7.0F * g1,
                    62.0F + 14.0F * g2,
                    78.0F + 14.0F * std::sin(w - 0.5F),
                    -24.0F + 10.0F * g2,
                    -6.0F,
                    8.0F * g1,
                    0.16F,
                    0.50F,
                    {0.25F, -0.35F, -0.90F}};
                s.arm_r = {
                    24.0F + 5.0F * g2,
                    38.0F + 10.0F * g1,
                    46.0F + 10.0F * g1,
                    -10.0F + 8.0F * g1,
                    0.0F,
                    0.0F,
                    0.19F,
                    0.40F,
                    {0.24F, -0.20F, -0.95F}};
                s.jaw_open = jaw;
                s.lip_upper_raise = 0.45F * jaw;
                s.lip_round = 0.35F * (0.5F + 0.5F * std::sin(2.0F * w));
                s.smile = 0.25F + 0.12F * g1;
                s.brow_lift = 0.45F * jaw + 0.15F * g1;
                s.blink = blink_env;
                return s;
            });
        clips.push_back(std::move(c));
    }

    // 12. attack (coiled wind-up -> dynamic forward horizontal slash -> balanced follow-through + hitboxes)
    {
        static constexpr std::array<std::pair<float, float>, 5U> atk_pelvis_yaw{{
            {0.00F, -3.0F}, {0.26F, -18.0F}, {0.48F, 16.0F}, {0.66F, 22.0F}, {1.00F, -2.0F},
        }};
        static constexpr std::array<std::pair<float, float>, 5U> atk_spine_yaw{{
            {0.00F, 3.0F}, {0.26F, -24.0F}, {0.46F, 26.0F}, {0.66F, 32.0F}, {1.00F, 2.0F},
        }};
        static constexpr std::array<std::pair<float, float>, 5U> atk_spine_pitch{{
            {0.00F, 2.0F}, {0.26F, -4.0F}, {0.46F, 13.0F}, {0.68F, 15.0F}, {1.00F, 2.0F},
        }};
        static constexpr std::array<std::pair<float, float>, 5U> atk_arm_r_el{{
            {0.00F, 24.0F}, {0.26F, 108.0F}, {0.45F, 90.0F}, {0.66F, 64.0F}, {1.00F, 18.0F},
        }};
        static constexpr std::array<std::pair<float, float>, 5U> atk_arm_r_az{{
            {0.00F, 28.0F}, {0.26F, -32.0F}, {0.45F, 82.0F}, {0.66F, 122.0F}, {1.00F, 16.0F},
        }};
        static constexpr std::array<std::pair<float, float>, 5U> atk_arm_r_flex{{
            {0.00F, 45.0F}, {0.26F, 72.0F}, {0.45F, 22.0F}, {0.66F, 38.0F}, {1.00F, 22.0F},
        }};
        static constexpr std::array<std::pair<float, float>, 5U> atk_lunge_lz{{
            {0.00F, 0.06F}, {0.26F, 0.07F}, {0.46F, 0.185F}, {0.68F, 0.17F}, {1.00F, 0.05F},
        }};
        static constexpr std::array<std::pair<float, float>, 5U> atk_trail_rz{{
            {0.00F, -0.04F}, {0.26F, -0.20F}, {0.46F, -0.22F}, {0.68F, -0.17F}, {1.00F, -0.02F},
        }};
        auto c = bake_human_clip(
            "attack", 0.75F, false, skeleton, metrics, false, false, false, true,
            [&](const float /*t*/, const float u) noexcept {
                const float strike = smoothstep01((u - 0.24F) / 0.24F) *
                    (1.0F - smoothstep01((u - 0.65F) / 0.35F));
                const float pel_yaw = eval_pchip_clamped(atk_pelvis_yaw, u);
                const float sp_yaw = eval_pchip_clamped(atk_spine_yaw, u);
                const float sp_pitch = eval_pchip_clamped(atk_spine_pitch, u);
                const float ar_el = eval_pchip_clamped(atk_arm_r_el, u);
                const float ar_az = eval_pchip_clamped(atk_arm_r_az, u);
                const float ar_fl = eval_pchip_clamped(atk_arm_r_flex, u);

                HumanPoseSpec s{};
                s.pelvis_offset = {
                    0.022F * strike * metrics.leg_ratio,
                    (-0.025F - 0.100F * strike) * metrics.leg_ratio,
                    0.075F * strike * metrics.leg_len};
                s.pelvis_euler_deg = {5.0F * strike + 1.5F, pel_yaw, -3.0F * strike};
                s.spine_euler_deg = {sp_pitch, sp_yaw, 2.5F * strike};
                s.head_euler_deg = {-0.7F * sp_pitch, -0.65F * (pel_yaw + sp_yaw), 0.0F};
                s.tailcoat_pitch_deg = 6.0F + 14.0F * strike;
                s.foot_l = {
                    metrics.stance_half_x * 1.15F,
                    0.0F,
                    eval_pchip_clamped(atk_lunge_lz, u) * metrics.leg_len,
                    8.0F,
                    0.0F,
                    0.07F};
                s.foot_r = {
                    metrics.stance_half_x * 1.25F,
                    0.0F,
                    eval_pchip_clamped(atk_trail_rz, u) * metrics.leg_len,
                    -18.0F,
                    18.0F * strike,
                    0.08F};
                s.arm_l = {
                    glm::mix(34.0F, 52.0F, 1.0F - strike),
                    glm::mix(58.0F, -48.0F, strike),
                    glm::mix(72.0F, 36.0F, strike),
                    14.0F,
                    0.0F,
                    0.0F,
                    0.55F,
                    0.15F,
                    {0.28F, -0.24F, -0.93F}};
                s.arm_r = {
                    ar_el,
                    ar_az,
                    ar_fl,
                    glm::mix(10.0F, 48.0F, strike),
                    4.0F * strike,
                    0.0F,
                    1.00F,
                    0.05F,
                    {0.46F, -0.64F, -0.61F}};
                s.jaw_open = 0.08F * strike;
                s.brow_lift = -0.80F * strike;
                return s;
            });
        c.events.push_back({0.25F, AnimationEventKind::hitbox_begin, "melee", "right_hand", 1.0F});
        c.events.push_back({0.48F, AnimationEventKind::hitbox_end, "melee", "right_hand", 1.0F});
        clips.push_back(std::move(c));
    }

    return clips;
}

SynthesizedSpeechClip synthesize_dialogue_speech(
    const std::string_view line_id,
    const std::string_view text,
    const VoiceProfileSpec& voice,
    const CookedSkeletonAsset& skeleton,
    const std::uint64_t seed) {
    SynthesizedSpeechClip result{};
    result.line_id = sanitize_character_slug(line_id.empty() ? "line" : line_id);
    result.sample_rate = 48000U;

    std::vector<PhonemeFrame> frames;
    std::size_t cursor = 0U;
    while (cursor < text.size()) {
        const std::uint32_t cp = next_utf8_codepoint(text, cursor);
        if (cp == 0U) break;
        frames.push_back(classify_codepoint(cp, voice.speech_rate));
    }
    if (frames.empty()) {
        frames.push_back(classify_codepoint('a', voice.speech_rate));
    }

    // Build viseme keyframes + phoneme timeline
    float total_dur = 0.0F;
    result.visemes.reserve(frames.size() + 2U);
    result.visemes.push_back(VisemeKeyframe{0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F});
    for (const PhonemeFrame& pf : frames) {
        const float mid = total_dur + pf.duration_s * 0.5F;
        result.visemes.push_back(VisemeKeyframe{
            .time_seconds = mid,
            .jaw_open = pf.jaw_open,
            .lip_round = pf.lip_round,
            .lip_wide = pf.lip_wide,
            .lip_pucker = pf.lip_pucker,
            .upper_lip_raise = pf.upper_lip,
        });
        total_dur += pf.duration_s;
    }
    total_dur = (std::min)(total_dur, 18.0F);
    result.visemes.push_back(VisemeKeyframe{total_dur, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F});
    result.duration_seconds = total_dur;

    // Synthesize 48 kHz mono formant speech waveform
    const auto total_samples = static_cast<std::size_t>(
        std::ceil(static_cast<double>(total_dur) * result.sample_rate));
    result.mono_pcm.resize(total_samples, 0.0F);

    const float base_f0 = clamp_finite(voice.pitch_hz, 70.0F, 340.0F, 135.0F);
    const bool is_question = !text.empty() && text.back() == '?';
    float phase_f0 = 0.0F;
    float p1 = 0.0F, p2 = 0.0F, p3 = 0.0F;
    std::uint32_t rng = static_cast<std::uint32_t>(seed ^ 0x9E3779B9U);
    float acc_time = 0.0F;
    std::size_t f_idx = 0U;

    const float inv_sr = 1.0F / static_cast<float>(result.sample_rate);
    for (std::size_t i = 0U; i < total_samples; ++i) {
        const float t = static_cast<float>(i) * inv_sr;
        while (f_idx + 1U < frames.size() && t > acc_time + frames[f_idx].duration_s) {
            acc_time += frames[f_idx].duration_s;
            ++f_idx;
        }
        const PhonemeFrame& pf = frames[f_idx];
        const float local_t = pf.duration_s > 1.0e-5F
            ? std::clamp((t - acc_time) / pf.duration_s, 0.0F, 1.0F)
            : 0.5F;
        const float syll_env = std::sin(local_t * glm::pi<float>());
        const float u_pos = total_dur > 1.0e-5F ? (t / total_dur) : 0.0F;
        const float pitch_contour =
            1.0F +
            (is_question ? (0.22F * u_pos - 0.06F) : (0.06F - 0.14F * u_pos)) +
            voice.cadence_variance * 0.07F * std::sin(u_pos * glm::two_pi<float>() * 2.5F);
        const float f0 = base_f0 * std::clamp(pitch_contour, 0.6F, 1.6F);
        phase_f0 = std::fmod(phase_f0 + f0 * inv_sr, 1.0F);
        p1 = std::fmod(p1 + pf.f1 * inv_sr, 1.0F);
        p2 = std::fmod(p2 + pf.f2 * inv_sr, 1.0F);
        p3 = std::fmod(p3 + pf.f3 * inv_sr, 1.0F);

        rng = rng * 1664525U + 1013904223U;
        const float noise =
            (static_cast<float>(rng & 0xFFFFU) / 32767.5F) - 1.0F;

        const float glottal =
            (0.65F + 0.35F * voice.warmth) * std::sin(phase_f0 * glm::two_pi<float>()) +
            0.35F * std::sin(phase_f0 * glm::two_pi<float>() * 2.0F);
        const float formant_sum =
            0.55F * std::sin(p1 * glm::two_pi<float>()) +
            0.30F * std::sin(p2 * glm::two_pi<float>()) +
            0.15F * std::sin(p3 * glm::two_pi<float>());
        const float voiced_sig = glottal * (0.55F + 0.45F * formant_sum) * pf.voiced;
        const float fric_sig = noise * (pf.fricative + voice.breathiness * 0.35F) * 0.25F;
        const float sample = (voiced_sig + fric_sig) * (0.35F + 0.65F * syll_env) * 0.45F;
        result.mono_pcm[i] = std::clamp(sample, -0.95F, 0.95F);
    }

    // Also build a cooked facial AnimationClip driving Jaw / Lip_Upper / Lip_Lower / Mouth_Corners
    AnimationClip fclip{};
    fclip.name = "speak_" + result.line_id;
    fclip.duration_seconds = total_dur;
    fclip.ticks_per_second = 30.0F;
    fclip.looping = false;
    const std::int32_t jaw_idx = skeleton.find_bone_index("Jaw");
    const std::int32_t lu_idx = skeleton.find_bone_index("Lip_Upper");
    const std::int32_t ll_idx = skeleton.find_bone_index("Lip_Lower");
    const std::int32_t mcl_idx = skeleton.find_bone_index("Mouth_Corner_L");
    const std::int32_t mcr_idx = skeleton.find_bone_index("Mouth_Corner_R");
    BoneAnimationTrack t_jaw{.bone_name = "Jaw", .bone_index = jaw_idx};
    BoneAnimationTrack t_lu{.bone_name = "Lip_Upper", .bone_index = lu_idx};
    BoneAnimationTrack t_ll{.bone_name = "Lip_Lower", .bone_index = ll_idx};
    BoneAnimationTrack t_mcl{.bone_name = "Mouth_Corner_L", .bone_index = mcl_idx};
    BoneAnimationTrack t_mcr{.bone_name = "Mouth_Corner_R", .bone_index = mcr_idx};
    const glm::vec3 lu_bind = lu_idx >= 0 ? skeleton.bones[static_cast<std::size_t>(lu_idx)].local_bind_translation : glm::vec3(0.0F);
    const glm::vec3 ll_bind = ll_idx >= 0 ? skeleton.bones[static_cast<std::size_t>(ll_idx)].local_bind_translation : glm::vec3(0.0F);
    const glm::vec3 mcl_bind = mcl_idx >= 0 ? skeleton.bones[static_cast<std::size_t>(mcl_idx)].local_bind_translation : glm::vec3(0.0F);
    const glm::vec3 mcr_bind = mcr_idx >= 0 ? skeleton.bones[static_cast<std::size_t>(mcr_idx)].local_bind_translation : glm::vec3(0.0F);
    for (const VisemeKeyframe& vk : result.visemes) {
        if (jaw_idx >= 0) {
            t_jaw.rotations.push_back(QuatKey{
                .time = vk.time_seconds,
                .value = glm::quat(glm::radians(glm::vec3(vk.jaw_open * 13.5F, 0.0F, 0.0F))),
            });
        }
        const float round_or_pucker = (std::max)(vk.lip_round, vk.lip_pucker);
        if (lu_idx >= 0) {
            t_lu.translations.push_back(Vec3Key{
                .time = vk.time_seconds,
                .value = lu_bind + glm::vec3(0.0F, vk.upper_lip_raise * 0.006F, round_or_pucker * 0.008F),
            });
        }
        if (ll_idx >= 0) {
            t_ll.translations.push_back(Vec3Key{
                .time = vk.time_seconds,
                .value = ll_bind + glm::vec3(0.0F, -vk.jaw_open * 0.0025F, round_or_pucker * 0.008F),
            });
        }
        const float corner_x = (vk.lip_wide - round_or_pucker) * 0.006F;
        const float corner_y = -vk.jaw_open * 0.0035F;
        if (mcl_idx >= 0) {
            t_mcl.translations.push_back(Vec3Key{
                .time = vk.time_seconds,
                .value = mcl_bind + glm::vec3(corner_x, corner_y, 0.0F),
            });
        }
        if (mcr_idx >= 0) {
            t_mcr.translations.push_back(Vec3Key{
                .time = vk.time_seconds,
                .value = mcr_bind + glm::vec3(-corner_x, corner_y, 0.0F),
            });
        }
    }
    if (jaw_idx >= 0) fclip.tracks.push_back(std::move(t_jaw));
    if (lu_idx >= 0) fclip.tracks.push_back(std::move(t_lu));
    if (ll_idx >= 0) fclip.tracks.push_back(std::move(t_ll));
    if (mcl_idx >= 0) fclip.tracks.push_back(std::move(t_mcl));
    if (mcr_idx >= 0) fclip.tracks.push_back(std::move(t_mcr));
    bind_clip_to_skeleton(fclip, skeleton);
    result.facial_clip = std::move(fclip);
    return result;
}

VisemeKeyframe sample_viseme_track(
    const std::span<const VisemeKeyframe> track,
    const float time_seconds) noexcept {
    if (track.empty()) {
        return VisemeKeyframe{};
    }
    if (time_seconds <= track.front().time_seconds) {
        return track.front();
    }
    if (time_seconds >= track.back().time_seconds) {
        return track.back();
    }
    for (std::size_t i = 0U; i + 1U < track.size(); ++i) {
        const VisemeKeyframe& a = track[i];
        const VisemeKeyframe& b = track[i + 1U];
        if (time_seconds <= b.time_seconds) {
            const float span = b.time_seconds - a.time_seconds;
            const float alpha = span > 1.0e-5F
                ? std::clamp((time_seconds - a.time_seconds) / span, 0.0F, 1.0F)
                : 0.0F;
            return VisemeKeyframe{
                .time_seconds = time_seconds,
                .jaw_open = glm::mix(a.jaw_open, b.jaw_open, alpha),
                .lip_round = glm::mix(a.lip_round, b.lip_round, alpha),
                .lip_wide = glm::mix(a.lip_wide, b.lip_wide, alpha),
                .lip_pucker = glm::mix(a.lip_pucker, b.lip_pucker, alpha),
                .upper_lip_raise = glm::mix(a.upper_lip_raise, b.upper_lip_raise, alpha),
            };
        }
    }
    return track.back();
}

void recompute_skeleton_pose_matrices(
    const CookedSkeletonAsset& skeleton,
    SkeletonPose& pose) noexcept {
    const std::size_t n = std::min<std::size_t>(skeleton.bones.size(), max_skeleton_bones);
    pose.bone_count = static_cast<std::uint32_t>(n);
    for (std::size_t i = 0U; i < n; ++i) {
        const BoneLocalTransform& l = pose.local[i];
        const glm::mat4 local_mat =
            glm::translate(glm::mat4(1.0F), l.translation) *
            glm::mat4_cast(l.rotation) *
            glm::scale(glm::mat4(1.0F), l.scale);
        const std::int32_t parent = skeleton.bones[i].parent_index;
        if (parent >= 0 && static_cast<std::size_t>(parent) < i) {
            pose.model_space[i] = pose.model_space[static_cast<std::size_t>(parent)] * local_mat;
        } else {
            pose.model_space[i] = local_mat;
        }
        pose.skin_matrices[i] =
            pose.model_space[i] * skeleton.bones[i].inverse_bind_matrix;
    }
}

void apply_facial_preview_pose(
    const CookedSkeletonAsset& skeleton,
    const FacialPreviewParams& facial,
    SkeletonPose& in_out_pose) noexcept {
    const auto add_trans = [&](const std::string_view name, const glm::vec3& off) {
        const std::int32_t idx = skeleton.find_bone_index(name);
        if (idx >= 0 && static_cast<std::size_t>(idx) < max_skeleton_bones) {
            in_out_pose.local[static_cast<std::size_t>(idx)].translation += off;
        }
    };
    const auto add_rot = [&](const std::string_view name, const glm::vec3& euler_deg) {
        const std::int32_t idx = skeleton.find_bone_index(name);
        if (idx >= 0 && static_cast<std::size_t>(idx) < max_skeleton_bones) {
            in_out_pose.local[static_cast<std::size_t>(idx)].rotation =
                in_out_pose.local[static_cast<std::size_t>(idx)].rotation *
                glm::quat(glm::radians(euler_deg));
        }
    };

    const float jaw = std::clamp(facial.jaw_open, 0.0F, 1.0F);
    const float smile = std::clamp(facial.smile, 0.0F, 1.0F);
    const float frown = std::clamp(facial.frown, 0.0F, 1.0F);
    const float brow = std::clamp(facial.brow_raise, 0.0F, 1.0F);
    const float blink = std::clamp(facial.eye_blink, 0.0F, 1.0F);

    add_rot("Jaw", {jaw * 13.5F, 0.0F, 0.0F});
    add_trans("Lip_Upper", {0.0F, facial.upper_lip_raise * 0.006F, facial.lip_round * 0.008F});
    add_trans("Lip_Lower", {0.0F, -jaw * 0.0025F, facial.lip_round * 0.008F});

    const float corner_y = (smile - frown) * 0.008F - jaw * 0.0035F;
    const float corner_x = (facial.lip_wide - facial.lip_round) * 0.008F;
    add_trans("Mouth_Corner_L", {corner_x, corner_y, -smile * 0.004F});
    add_trans("Mouth_Corner_R", {-corner_x, corner_y, -smile * 0.004F});

    add_rot("Eye_L", {facial.eye_look.y * -14.0F, facial.eye_look.x * 20.0F, 0.0F});
    add_rot("Eye_R", {facial.eye_look.y * -14.0F, facial.eye_look.x * 20.0F, 0.0F});

    add_trans("Eyelid_Upper_L", {0.0F, -blink * 0.015F + brow * 0.003F, blink * 0.002F});
    add_trans("Eyelid_Upper_R", {0.0F, -blink * 0.015F + brow * 0.003F, blink * 0.002F});
    add_trans("Brow_L", {0.0F, brow * 0.008F - frown * 0.006F, 0.0F});
    add_trans("Brow_R", {0.0F, brow * 0.008F - frown * 0.006F, 0.0F});
    add_trans("Brow_Center", {0.0F, brow * 0.005F - frown * 0.007F, 0.0F});

    recompute_skeleton_pose_matrices(skeleton, in_out_pose);
}

void evaluate_character_preview_pose(
    const CookedSkeletonAsset& skeleton,
    const std::span<const AnimationClip> clips,
    const std::string_view clip_name,
    const float time_seconds,
    const FacialPreviewParams& facial,
    SkeletonPose& out_pose) noexcept {
    const AnimationClip* found = nullptr;
    for (const AnimationClip& c : clips) {
        if (c.name == clip_name) {
            found = &c;
            break;
        }
    }
    if (found != nullptr) {
        sample_animation_clip(skeleton, *found, time_seconds, found->looping, out_pose);
    } else {
        set_bind_pose(skeleton, out_pose);
    }
    apply_facial_preview_pose(skeleton, facial, out_pose);
}

CharacterRaycastHit raycast_character_mesh(
    const CharacterMeshBuildResult& mesh,
    const glm::vec3& ray_origin,
    const glm::vec3& ray_direction) noexcept {
    CharacterRaycastHit best{};
    const float dir_len = glm::length(ray_direction);
    if (dir_len < 1.0e-6F || mesh.indices.size() < 3U) {
        return best;
    }
    const glm::vec3 dir = ray_direction / dir_len;
    float best_t = std::numeric_limits<float>::infinity();

    for (const CharacterSubmeshBuild& sub : mesh.submeshes) {
        const std::uint32_t end = sub.first_index + sub.index_count;
        for (std::uint32_t i = sub.first_index; i + 2U < end && i + 2U < mesh.indices.size(); i += 3U) {
            const std::uint32_t i0 = mesh.indices[i];
            const std::uint32_t i1 = mesh.indices[i + 1U];
            const std::uint32_t i2 = mesh.indices[i + 2U];
            if (i0 >= mesh.vertices.size() || i1 >= mesh.vertices.size() || i2 >= mesh.vertices.size()) {
                continue;
            }
            const glm::vec3 v0(mesh.vertices[i0].position[0], mesh.vertices[i0].position[1], mesh.vertices[i0].position[2]);
            const glm::vec3 v1(mesh.vertices[i1].position[0], mesh.vertices[i1].position[1], mesh.vertices[i1].position[2]);
            const glm::vec3 v2(mesh.vertices[i2].position[0], mesh.vertices[i2].position[1], mesh.vertices[i2].position[2]);

            const glm::vec3 e1 = v1 - v0;
            const glm::vec3 e2 = v2 - v0;
            const glm::vec3 h = glm::cross(dir, e2);
            const float a = glm::dot(e1, h);
            if (std::abs(a) < 1.0e-6F) {
                continue;
            }
            const float f = 1.0F / a;
            const glm::vec3 s = ray_origin - v0;
            const float u = f * glm::dot(s, h);
            if (u < 0.0F || u > 1.0F) continue;
            const glm::vec3 q = glm::cross(s, e1);
            const float v = f * glm::dot(dir, q);
            if (v < 0.0F || u + v > 1.0F) continue;
            const float t = f * glm::dot(e2, q);
            if (t > 1.0e-4F && t < best_t) {
                best_t = t;
                const float w = 1.0F - u - v;
                best.hit = true;
                best.distance = t;
                best.position = ray_origin + dir * t;
                const glm::vec3 fn = glm::cross(e1, e2);
                best.normal = glm::length(fn) > 1.0e-6F ? glm::normalize(fn) : glm::vec3(0.0F, 1.0F, 0.0F);
                best.uv = glm::vec2(
                    w * mesh.vertices[i0].texcoord[0] + u * mesh.vertices[i1].texcoord[0] + v * mesh.vertices[i2].texcoord[0],
                    1.0F - (w * mesh.vertices[i0].texcoord[1] + u * mesh.vertices[i1].texcoord[1] + v * mesh.vertices[i2].texcoord[1]));
                best.target = sub.paint_target;
                best.triangle_index = i / 3U;
            }
        }
    }
    return best;
}

// ============================================================================
// Binary Writers & Atomic Project Cooker
// ============================================================================

bool write_cooked_mesh_file(
    const std::filesystem::path& path,
    const std::span<const CookedModelVertex> vertices,
    const std::span<const std::uint32_t> indices,
    const std::span<const CharacterSubmeshBuild> submeshes,
    std::string& error) {
    error.clear();
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) {
            error = "Failed to create mesh directory '" + utf8_from_path(path.parent_path()) + "'.";
            return false;
        }
    }
    const std::filesystem::path temp = path.string() + ".tmp_vcrmesh";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "Failed to open '" + utf8_from_path(temp) + "' for mesh write.";
            return false;
        }
        constexpr std::array<char, 8U> magic{'V', 'C', 'R', 'M', 'E', 'S', 'H', '\0'};
        out.write(magic.data(), static_cast<std::streamsize>(magic.size()));
        write_u32_le(out, 1U);
        write_u32_le(out, static_cast<std::uint32_t>(cooked_mesh_vertex_stride));
        write_u64_le(out, static_cast<std::uint64_t>(vertices.size()));
        write_u64_le(out, static_cast<std::uint64_t>(indices.size()));
        write_u32_le(out, static_cast<std::uint32_t>(submeshes.size()));
        write_u32_le(out, 0U);

        for (const CharacterSubmeshBuild& sub : submeshes) {
            write_u64_le(out, sub.first_index);
            write_u64_le(out, sub.index_count);
            write_u32_le(out, sub.material_index);
            write_u32_le(out, 0U);
        }
        for (const CookedModelVertex& v : vertices) {
            for (const float f : v.position) write_f32_le(out, f);
            for (const float f : v.normal) write_f32_le(out, f);
            for (const float f : v.texcoord) write_f32_le(out, f);
            for (const float f : v.tangent) write_f32_le(out, f);
        }
        for (const std::uint32_t idx : indices) {
            write_u32_le(out, idx);
        }
        out.flush();
        if (!out) {
            error = "I/O error writing mesh '" + utf8_from_path(temp) + "'.";
            out.close();
            std::filesystem::remove(temp, ec);
            return false;
        }
    }
    std::filesystem::rename(temp, path, ec);
    if (ec) {
#if defined(_WIN32)
        if (MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0) {
            return true;
        }
#endif
        error = "Failed to rename cooked mesh '" + utf8_from_path(path) + "'.";
        std::filesystem::remove(temp, ec);
        return false;
    }
    return true;
}

bool write_pcm_wav_file(
    const std::filesystem::path& path,
    const std::span<const float> mono_pcm,
    const std::uint32_t sample_rate,
    std::string& error) {
    error.clear();
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    const std::uint32_t sr = std::clamp(sample_rate, 8000U, 96000U);
    const auto data_bytes = static_cast<std::uint32_t>(mono_pcm.size() * 2U);
    const std::filesystem::path temp = path.string() + ".tmp_wav";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "Failed to open '" + utf8_from_path(temp) + "' for WAV write.";
            return false;
        }
        out.write("RIFF", 4);
        write_u32_le(out, 36U + data_bytes);
        out.write("WAVE", 4);
        out.write("fmt ", 4);
        write_u32_le(out, 16U);
        write_u16_le(out, 1U); // PCM
        write_u16_le(out, 1U); // Mono
        write_u32_le(out, sr);
        write_u32_le(out, sr * 2U);
        write_u16_le(out, 2U);
        write_u16_le(out, 16U);
        out.write("data", 4);
        write_u32_le(out, data_bytes);
        for (const float s : mono_pcm) {
            const float clamped = std::clamp(s, -1.0F, 1.0F);
            const auto i16 = static_cast<std::int16_t>(std::lround(clamped * 32767.0F));
            write_i16_le(out, i16);
        }
        out.flush();
        if (!out) {
            error = "I/O error writing WAV '" + utf8_from_path(temp) + "'.";
            out.close();
            std::filesystem::remove(temp, ec);
            return false;
        }
    }
    std::filesystem::rename(temp, path, ec);
    if (ec) {
#if defined(_WIN32)
        if (MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0) {
            return true;
        }
#endif
        error = "Failed to commit WAV '" + utf8_from_path(path) + "'.";
        std::filesystem::remove(temp, ec);
        return false;
    }
    return true;
}

bool cook_character_package_to_project(
    const CharacterPackage& input_pkg,
    const std::filesystem::path& project_directory,
    const CharacterCookOptions& options,
    CharacterCookReport& out_report,
    std::string& error) {
    error.clear();
    out_report = CharacterCookReport{};
    CharacterPackage pkg = input_pkg;
    sanitize_character_package(pkg);
    const std::string& slug = pkg.identity.slug;
    out_report.character_slug = slug;

    const std::filesystem::path asset_root = project_directory / "assets";
    std::error_code ec;
    std::filesystem::create_directories(asset_root / "characters", ec);
    if (ec) {
        error = "Cannot create project assets/characters directory: " + ec.message();
        return false;
    }

    const std::uint64_t stamp = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const std::filesystem::path staging_dir =
        asset_root / "characters" / (".staging_" + slug + "_" + std::to_string(stamp));
    const std::filesystem::path final_dir = asset_root / "characters" / slug;
    const std::filesystem::path backup_dir =
        asset_root / "characters" / (".backup_" + slug + "_" + std::to_string(stamp));

    if (!options.overwrite_existing && std::filesystem::exists(final_dir, ec)) {
        error = "Character asset directory already exists and overwrite is disabled.";
        return false;
    }

    std::filesystem::create_directories(staging_dir / "source", ec);
    std::filesystem::create_directories(staging_dir / "mesh", ec);
    std::filesystem::create_directories(staging_dir / "materials", ec);
    std::filesystem::create_directories(staging_dir / "textures", ec);
    std::filesystem::create_directories(staging_dir / "animations", ec);
    if (ec) {
        error = "Failed to create character staging subdirectories.";
        std::filesystem::remove_all(staging_dir, ec);
        return false;
    }

    // Save source snapshot of the .vcrchar.json inside source/<slug>.vcrchar.json
    const std::string source_rel = "source/" + slug + ".vcrchar.json";
    if (!save_character_package_file(staging_dir / source_rel, pkg, error)) {
        std::filesystem::remove_all(staging_dir, ec);
        return false;
    }
    ++out_report.cooked_file_count;

    // 1. Build 50-bone skeleton + mesh + textures
    const CharacterMeshBuildResult mesh = build_character_mesh(pkg);
    if (mesh.vertices.empty() || mesh.indices.empty() || mesh.submeshes.empty()) {
        error = "Procedural character mesh produced zero geometry.";
        std::filesystem::remove_all(staging_dir, ec);
        return false;
    }
    const auto baked_textures = bake_character_textures(
        pkg, mesh, options.texture_resolution);

    out_report.vertex_count = static_cast<std::uint32_t>(mesh.vertices.size());
    out_report.triangle_count = static_cast<std::uint32_t>(mesh.indices.size() / 3U);
    out_report.bone_count = static_cast<std::uint32_t>(mesh.skeleton.bones.size());
    out_report.submesh_count = static_cast<std::uint32_t>(mesh.submeshes.size());

    // 2. Write .vcrmesh and .vcrskin
    const std::string mesh_rel = "mesh/" + slug + ".vcrmesh";
    const std::string skin_rel = "mesh/" + slug + ".vcrskin";
    if (!write_cooked_mesh_file(
            staging_dir / mesh_rel, mesh.vertices, mesh.indices, mesh.submeshes, error) ||
        !write_cooked_skin_file(
            staging_dir / skin_rel, mesh.skeleton, mesh.skin_vertices, error)) {
        std::filesystem::remove_all(staging_dir, ec);
        return false;
    }
    out_report.cooked_file_count += 2U;

    // 3. Write textures (PNG + .vcrtexture.json) & materials (.vcrmaterial.json)
    Json materials_array = Json::array();
    Json submesh_array = Json::array();
    for (std::size_t si = 0U; si < mesh.submeshes.size(); ++si) {
        const CharacterSubmeshBuild& sub = mesh.submeshes[si];
        const std::string& slot = sub.name;

        const std::string bc_png_rel = "source/" + slot + "_base_color.png";
        const std::string nrm_png_rel = "source/" + slot + "_normal.png";
        const std::string mr_png_rel = "source/" + slot + "_metallic_roughness.png";
        const std::string bc_tex_rel = "textures/" + slot + "_base_color.vcrtexture.json";
        const std::string nrm_tex_rel = "textures/" + slot + "_normal.vcrtexture.json";
        const std::string mr_tex_rel = "textures/" + slot + "_metallic_roughness.vcrtexture.json";

        if (si < baked_textures.size()) {
            const BakedPbrTextureSet& bt = baked_textures[si];
            const auto write_tex_pair = [&](
                const std::string& png_rel,
                const std::string& json_rel,
                const std::string& tex_name,
                const std::string_view usage,
                const std::string_view color_space,
                const std::vector<std::uint8_t>& rgba) -> bool {
                const PngEncodeResult png = encode_png_rgba(rgba, bt.width, bt.height);
                if (!png) {
                    error = "Failed to encode character PNG texture '" + tex_name +
                        "': " + png.error;
                    return false;
                }
                if (!write_bytes_file_atomic(staging_dir / png_rel, png.bytes, error)) {
                    return false;
                }
                const Json tj{
                    {"schema", "vcr.asset"},
                    {"version", 1U},
                    {"kind", "texture"},
                    {"name", slug + "_" + tex_name},
                    {"origin", "generated"},
                    {"source", png_rel},
                    {"source_format", "png"},
                    {"usage", usage},
                    {"color_space", color_space},
                    {"decode", "deferred"},
                    {"width", bt.width},
                    {"height", bt.height},
                };
                if (!write_text_file_atomic(
                        staging_dir / json_rel, tj.dump(2) + "\n", error)) {
                    return false;
                }
                out_report.cooked_file_count += 2U;
                return true;
            };
            if (!write_tex_pair(bc_png_rel, bc_tex_rel, slot + "_base_color", "base_color", "srgb", bt.base_color_rgba) ||
                !write_tex_pair(nrm_png_rel, nrm_tex_rel, slot + "_normal", "normal", "linear", bt.normal_rgba) ||
                !write_tex_pair(mr_png_rel, mr_tex_rel, slot + "_mr", "metallic_roughness", "linear", bt.metallic_roughness_rgba)) {
                std::filesystem::remove_all(staging_dir, ec);
                return false;
            }
        }

        const std::string mat_rel = "materials/" + slot + ".vcrmaterial.json";
        const bool is_outfit_slot =
            (slot == "outfit_primary" || slot == "outfit_secondary");
        const bool texture_driven_pbr =
            (slot == "skin" || slot == "eyes" || slot == "hair" || is_outfit_slot);
        Json pbr_json{
            {"base_color", {1.0F, 1.0F, 1.0F, sub.material.base_color.a}},
            {"metallic", texture_driven_pbr ? 1.0F : sub.material.metallic},
            {"roughness", texture_driven_pbr ? 1.0F : sub.material.roughness},
            {"ambient_occlusion", sub.material.ambient_occlusion},
            {"ior", sub.material.ior},
            {"model", to_token(sub.material.model)},
            {"subsurface_profile", to_token(sub.material.subsurface_profile)},
            {"subsurface_strength", sub.material.subsurface_strength},
        };
        Json mat_json{
            {"schema", "vcr.asset"},
            {"version", 1U},
            {"kind", "material"},
            {"name", slot},
            {"pbr", std::move(pbr_json)},
            {"textures",
             {
                 {"base_color", bc_tex_rel},
                 {"normal", nrm_tex_rel},
                 {"metallic_roughness", mr_tex_rel},
             }},
        };
        if (slot == "skin" || slot == "hair" || is_outfit_slot) {
            mat_json["occlusion_in_metallic_roughness"] = true;
        }
        if (slot == "hair" || is_outfit_slot) {
            mat_json["double_sided"] = true;
        } else if (slot == "cornea") {
            mat_json["glass"] = true;
            mat_json["double_sided"] = false;
        }
        if (!write_text_file_atomic(
                staging_dir / mat_rel, mat_json.dump(2) + "\n", error)) {
            std::filesystem::remove_all(staging_dir, ec);
            return false;
        }
        ++out_report.cooked_file_count;

        materials_array.push_back(mat_rel);
        submesh_array.push_back(Json{
            {"name", slot},
            {"first_index", sub.first_index},
            {"index_count", sub.index_count},
            {"material_index", sub.material_index},
            {"material", mat_rel},
        });
    }

    // 4. Write all animations (.vcranim)
    std::vector<AnimationClip> clips =
        generate_character_animation_clips(pkg, mesh.skeleton);

    // 5. Write voice WAV + .vcraudio.json AND extract spoken facial clips
    std::vector<std::filesystem::path> staged_audio_rel;
    if (options.bake_voice_lines) {
        const std::filesystem::path voice_dir =
            asset_root / "audio" / "voices" / slug;
        std::filesystem::create_directories(voice_dir, ec);
        for (const DialogueNodeSpec& node : pkg.dialogue.nodes) {
            const SynthesizedSpeechClip speech = synthesize_dialogue_speech(
                node.voice_clip_id,
                node.text,
                pkg.voice,
                mesh.skeleton,
                pkg.identity.seed);
            const std::string wav_name = speech.line_id + ".wav";
            const std::string aud_name = speech.line_id + ".vcraudio.json";
            if (!write_pcm_wav_file(
                    voice_dir / wav_name,
                    speech.mono_pcm,
                    speech.sample_rate,
                    error)) {
                std::filesystem::remove_all(staging_dir, ec);
                return false;
            }
            const Json aud_json{
                {"schema", "vcr.asset"},
                {"version", 1U},
                {"kind", "audio"},
                {"name", slug + "_" + speech.line_id},
                {"source", wav_name},
                {"stream_recommended", false},
            };
            if (!write_text_file_atomic(
                    voice_dir / aud_name, aud_json.dump(2) + "\n", error)) {
                std::filesystem::remove_all(staging_dir, ec);
                return false;
            }
            staged_audio_rel.push_back(
                std::filesystem::path("audio") / "voices" / slug / aud_name);
            out_report.cooked_file_count += 2U;

            if (!speech.facial_clip.tracks.empty()) {
                clips.push_back(speech.facial_clip);
            }
        }
    }

    Json anims_array = Json::array();
    for (const AnimationClip& clip : clips) {
        const std::string clip_slug = sanitize_character_slug(clip.name);
        const std::string anim_rel = "animations/" + clip_slug + ".vcranim";
        if (!write_cooked_anim_file(staging_dir / anim_rel, clip, error)) {
            std::filesystem::remove_all(staging_dir, ec);
            return false;
        }
        ++out_report.cooked_file_count;
        anims_array.push_back(Json{
            {"name", clip.name},
            {"path", anim_rel},
            {"duration_seconds", clip.duration_seconds},
        });
        out_report.animation_clips_relative.push_back(
            std::filesystem::path("characters") / slug / anim_rel);
    }

    // 6. Write primary <slug>.vcrasset.json
    const glm::vec3 b_center = 0.5F * (mesh.bounds_min + mesh.bounds_max);
    const glm::vec3 b_extents = glm::max(
        0.5F * (mesh.bounds_max - mesh.bounds_min), glm::vec3(0.05F));
    const std::string asset_json_rel = slug + ".vcrasset.json";
    const Json model_json{
        {"schema", "vcr.asset"},
        {"version", 1U},
        {"kind", "model"},
        {"name", slug},
        {"source", source_rel},
        {"source_format", "vcrchar"},
        {"mesh", mesh_rel},
        {"skeleton", skin_rel},
        {"animations", std::move(anims_array)},
        {"geometry",
         {
             {"vertex_count", mesh.vertices.size()},
             {"index_count", mesh.indices.size()},
             {"index_type", "uint32"},
             {"primitive", "triangles"},
             {"vertex_stride", cooked_mesh_vertex_stride},
             {"vertex_layout",
              {
                  {"position", "float3"},
                  {"normal", "float3"},
                  {"texcoord0", "float2"},
                  {"tangent", "float4"},
              }},
         }},
        {"submeshes", std::move(submesh_array)},
        {"materials", std::move(materials_array)},
        {"collision",
         {
             {"local_bounds",
              {
                  {"center", {b_center.x, b_center.y, b_center.z}},
                  {"half_extents", {b_extents.x, b_extents.y, b_extents.z}},
              }},
             {"boxes",
              Json::array({Json{
                  {"center", {b_center.x, b_center.y, b_center.z}},
                  {"half_extents", {b_extents.x, b_extents.y, b_extents.z}},
              }})},
             {"triangle_count", mesh.indices.size() / 3U},
         }},
        {"coordinate_space", "rigged_root_space"},
    };
    if (!write_text_file_atomic(
            staging_dir / asset_json_rel, model_json.dump(2) + "\n", error)) {
        std::filesystem::remove_all(staging_dir, ec);
        return false;
    }
    ++out_report.cooked_file_count;

    // 7. Atomically promote staging_dir -> final_dir
    bool had_existing = false;
    if (std::filesystem::exists(final_dir, ec)) {
        had_existing = true;
        std::filesystem::rename(final_dir, backup_dir, ec);
        if (ec) {
            error = "Unable to rotate existing character directory '" +
                    utf8_from_path(final_dir) + "': " + ec.message();
            std::filesystem::remove_all(staging_dir, ec);
            return false;
        }
    }
    std::filesystem::rename(staging_dir, final_dir, ec);
    if (ec) {
        error = "Unable to publish staged character directory '" +
                utf8_from_path(final_dir) + "': " + ec.message();
        std::error_code revert_ec;
        if (had_existing) {
            std::filesystem::rename(backup_dir, final_dir, revert_ec);
        }
        std::filesystem::remove_all(staging_dir, revert_ec);
        return false;
    }
    if (had_existing) {
        std::filesystem::remove_all(backup_dir, ec);
    }

    out_report.model_asset_relative =
        std::filesystem::path("characters") / slug / asset_json_rel;
    out_report.audio_assets_relative = std::move(staged_audio_rel);

    // 8. Publish Dialogue Graph (.vcrdlg.json) compatible with GameUiRuntime
    {
        const std::filesystem::path dlg_rel =
            std::filesystem::path("dialogues") / (slug + ".vcrdlg.json");
        Json dlg_nodes = Json::array();
        for (const DialogueNodeSpec& n : pkg.dialogue.nodes) {
            Json dchoices = Json::array();
            for (const DialogueChoiceSpec& c : n.choices) {
                Json cj{
                    {"text", c.text},
                    {"close", c.closes_dialogue || c.next_node_id.empty()},
                };
                if (!c.next_node_id.empty()) {
                    cj["next"] = c.next_node_id;
                }
                if (!c.set_var.empty()) {
                    cj["set_var"] = c.set_var;
                    cj["value"] = c.set_value;
                }
                dchoices.push_back(std::move(cj));
            }
            dlg_nodes.push_back(Json{
                {"id", n.id},
                {"speaker", n.speaker},
                {"text", n.text},
                {"choices", std::move(dchoices)},
            });
        }
        const Json dlg_json{
            {"dialogue_id", pkg.dialogue.dialogue_id},
            {"entry_node", pkg.dialogue.entry_node_id},
            {"nodes", std::move(dlg_nodes)},
        };
        if (!write_text_file_atomic(
                asset_root / dlg_rel, dlg_json.dump(2) + "\n", error)) {
            return false;
        }
        out_report.dialogue_relative = dlg_rel;
        ++out_report.cooked_file_count;
    }

    // 9. Publish Localization (.vcrloc.json EN & RU)
    if (options.generate_localization) {
        Json en_entries = Json::object();
        Json ru_entries = Json::object();
        en_entries["char." + slug + ".name"] = pkg.identity.display_name;
        ru_entries["char." + slug + ".name"] = pkg.identity.display_name;
        for (const DialogueNodeSpec& n : pkg.dialogue.nodes) {
            en_entries["dlg." + slug + "." + n.id + ".text"] = n.text;
            ru_entries["dlg." + slug + "." + n.id + ".text"] =
                n.text_ru.empty() ? n.text : n.text_ru;
            for (std::size_t ci = 0U; ci < n.choices.size(); ++ci) {
                const std::string ckey =
                    "dlg." + slug + "." + n.id + ".choice_" + std::to_string(ci);
                en_entries[ckey] = n.choices[ci].text;
                ru_entries[ckey] = n.choices[ci].text_ru.empty()
                    ? n.choices[ci].text
                    : n.choices[ci].text_ru;
            }
        }
        const Json en_doc{
            {"format", "vcr_localization_v1"},
            {"locale", "en"},
            {"display_name", "English"},
            {"entries", std::move(en_entries)},
        };
        const Json ru_doc{
            {"format", "vcr_localization_v1"},
            {"locale", "ru"},
            {"display_name", "Русский"},
            {"entries", std::move(ru_entries)},
        };
        const std::filesystem::path en_rel =
            std::filesystem::path("localization") / (slug + "_en.vcrloc.json");
        const std::filesystem::path ru_rel =
            std::filesystem::path("localization") / (slug + "_ru.vcrloc.json");
        if (!write_text_file_atomic(asset_root / en_rel, en_doc.dump(2) + "\n", error) ||
            !write_text_file_atomic(asset_root / ru_rel, ru_doc.dump(2) + "\n", error)) {
            return false;
        }
        out_report.loc_en_relative = en_rel;
        out_report.loc_ru_relative = ru_rel;
        out_report.cooked_file_count += 2U;
    }

    // 10. Publish NPC & Player Prefabs (.vcrprefab.json)
    if (options.generate_prefabs) {
        std::filesystem::create_directories(asset_root / "prefabs", ec);
        const std::string asset_rel_str =
            out_report.model_asset_relative.generic_string();

        const auto build_and_save_prefab = [&](const bool is_player,
                                               const std::string& prefab_rel) -> bool {
            SceneDocument doc;
            const SceneNodeId root = doc.create_node(
                pkg.identity.display_name, SceneNodeKind::model);
            if (!root) {
                error = "Failed to allocate SceneDocument node for prefab.";
                return false;
            }
            if (const auto node_opt = doc.node(root)) {
                node_opt->get().asset_path = asset_rel_str;
            }
            ColliderComponent col{};
            col.shape = ColliderShape::capsule;
            col.radius = 0.36F;
            col.half_height = std::max(0.25F, (pkg.body.height_meters * 0.5F) - col.radius);
            col.center_offset = {0.0F, pkg.body.height_meters * 0.5F, 0.0F};
            static_cast<void>(doc.set_collider(root, col));

            AnimatorComponent anim{};
            anim.mode = AnimatorBlendMode::single_clip;
            static_cast<void>(anim.set_clip_name(pkg.acting.default_idle_clip));
            anim.looping = true;
            anim.playing = true;
            anim.foot_ik_enabled = true;
            anim.look_at_ik_enabled = true;
            static_cast<void>(doc.set_animator(root, anim));

            GameplayComponent gp{};
            static_cast<void>(gp.set_behavior(is_player ? "player_character" : "npc_dialogue"));
            static_cast<void>(gp.set_tag(is_player ? "player" : slug));
            gp.move_speed = pkg.acting.run_speed_mps;
            gp.interaction_radius = 2.4F;
            static_cast<void>(doc.set_gameplay(root, gp));

            if (!is_player) {
                NpcAiComponent npc{};
                npc.walk_speed = pkg.acting.walk_speed_mps;
                npc.run_speed = pkg.acting.run_speed_mps;
                npc.agent_height = pkg.body.height_meters;
                static_cast<void>(npc.set_idle_clip(pkg.acting.default_idle_clip));
                static_cast<void>(npc.set_walk_clip("walk"));
                static_cast<void>(npc.set_run_clip("run"));
                static_cast<void>(npc.set_jump_clip("jump"));
                static_cast<void>(npc.set_attack_clip("attack"));
                static_cast<void>(doc.set_npc_ai(root, npc));
            }
            return save_prefab_file(doc, root, asset_root, prefab_rel, error);
        };

        const std::string npc_rel = "prefabs/" + slug + "_npc.vcrprefab.json";
        const std::string ply_rel = "prefabs/" + slug + "_player.vcrprefab.json";
        if (!build_and_save_prefab(false, npc_rel) ||
            !build_and_save_prefab(true, ply_rel)) {
            return false;
        }
        out_report.npc_prefab_relative = npc_rel;
        out_report.player_prefab_relative = ply_rel;
        out_report.cooked_file_count += 2U;
    }

    return true;
}

bool verify_cooked_character_outputs(
    const std::filesystem::path& project_directory,
    const CharacterCookReport& report,
    std::string& error) {
    error.clear();
    const std::filesystem::path asset_root = project_directory / "assets";

    const auto model_res = load_cooked_model_asset(
        asset_root, report.model_asset_relative);
    if (!model_res) {
        error = "Model verification failed: " + model_res.error().message;
        return false;
    }
    if (model_res.value().skeleton.bones.size() != character_total_bone_count) {
        error = "Expected 50-bone skeleton, got " +
            std::to_string(model_res.value().skeleton.bones.size());
        return false;
    }
    if (model_res.value().animations.size() < 12U) {
        error = "Expected at least 12 locomotion/gesture clips.";
        return false;
    }

    const std::filesystem::path tex_dir =
        asset_root / "characters" / report.character_slug / "textures";
    std::error_code tex_ec;
    std::size_t verified_textures = 0U;
    if (std::filesystem::exists(tex_dir, tex_ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(tex_dir, tex_ec)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            const std::filesystem::path rel =
                std::filesystem::relative(entry.path(), asset_root, tex_ec);
            if (tex_ec) {
                error = "Failed to compute relative texture path.";
                return false;
            }
            const auto tex_res = load_cooked_texture_asset(asset_root, rel);
            if (!tex_res) {
                error = "Texture verification failed (" + rel.generic_string() + "): " +
                    tex_res.error().message;
                return false;
            }
            ++verified_textures;
        }
    }
    if (verified_textures < 3U) {
        error = "Expected at least 3 baked PBR textures for character '" +
            report.character_slug + "'.";
        return false;
    }

    if (!report.dialogue_relative.empty()) {
        GameUiRuntime ui_runtime;
        std::string dlg_err;
        if (!ui_runtime.load_dialogue_file(asset_root / report.dialogue_relative, &dlg_err)) {
            error = "Dialogue verification failed: " + dlg_err;
            return false;
        }
    }

    if (!report.loc_en_relative.empty()) {
        LocalizationSystem loc;
        std::string loc_err;
        if (!loc.load_file(asset_root / report.loc_en_relative, &loc_err) ||
            !loc.load_file(asset_root / report.loc_ru_relative, &loc_err)) {
            error = "Localization verification failed: " + loc_err;
            return false;
        }
    }

    for (const auto& audio_rel : report.audio_assets_relative) {
        CookedAudioAsset audio_asset{};
        if (!load_cooked_audio_asset(
                asset_root, audio_rel.generic_string(), audio_asset, error)) {
            error = "Audio verification failed (" + audio_rel.generic_string() + "): " + error;
            return false;
        }
    }

    if (!report.npc_prefab_relative.empty()) {
        SceneDocument scene;
        PrefabInstantiateOptions popts{};
        PrefabInstantiateResult pres{};
        if (!instantiate_prefab_file(
                scene, asset_root, report.npc_prefab_relative.generic_string(),
                popts, pres, error)) {
            error = "NPC prefab verification failed: " + error;
            return false;
        }
    }

    if (!report.player_prefab_relative.empty()) {
        SceneDocument scene;
        PrefabInstantiateOptions popts{};
        PrefabInstantiateResult pres{};
        if (!instantiate_prefab_file(
                scene, asset_root, report.player_prefab_relative.generic_string(),
                popts, pres, error)) {
            error = "Player prefab verification failed: " + error;
            return false;
        }
    }
    return true;
}

} // namespace vcr
