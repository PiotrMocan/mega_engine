#include "game_overlay.hpp"
#include "camera_projection.hpp"
#include "png_encoder.hpp"

#include <nlohmann/json.hpp>
#include <stb_image.h>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/vec4.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <memory>

namespace vcr {

namespace {

// 5x7 bitmap glyphs stored as 7 rows of 5 bits (LSB = rightmost column, bit 4 = leftmost).
struct GlyphBitmap {
    std::array<std::uint8_t, 7U> rows{};
};

constexpr std::uint32_t decode_utf8(std::string_view text, std::size_t& offset) noexcept {
    if (offset >= text.size()) {
        return 0U;
    }
    const std::uint8_t b0 = static_cast<std::uint8_t>(text[offset]);
    if (b0 < 0x80U) {
        ++offset;
        return b0;
    }
    if ((b0 & 0xE0U) == 0xC0U && offset + 1U < text.size()) {
        const std::uint8_t b1 = static_cast<std::uint8_t>(text[offset + 1U]);
        if ((b1 & 0xC0U) == 0x80U) {
            offset += 2U;
            return (static_cast<std::uint32_t>(b0 & 0x1FU) << 6U) |
                static_cast<std::uint32_t>(b1 & 0x3FU);
        }
    }
    if ((b0 & 0xF0U) == 0xE0U && offset + 2U < text.size()) {
        const std::uint8_t b1 = static_cast<std::uint8_t>(text[offset + 1U]);
        const std::uint8_t b2 = static_cast<std::uint8_t>(text[offset + 2U]);
        if ((b1 & 0xC0U) == 0x80U && (b2 & 0xC0U) == 0x80U) {
            offset += 3U;
            return (static_cast<std::uint32_t>(b0 & 0x0FU) << 12U) |
                (static_cast<std::uint32_t>(b1 & 0x3FU) << 6U) |
                static_cast<std::uint32_t>(b2 & 0x3FU);
        }
    }
    if ((b0 & 0xF8U) == 0xF0U && offset + 3U < text.size()) {
        const std::uint8_t b1 = static_cast<std::uint8_t>(text[offset + 1U]);
        const std::uint8_t b2 = static_cast<std::uint8_t>(text[offset + 2U]);
        const std::uint8_t b3 = static_cast<std::uint8_t>(text[offset + 3U]);
        if ((b1 & 0xC0U) == 0x80U && (b2 & 0xC0U) == 0x80U && (b3 & 0xC0U) == 0x80U) {
            offset += 4U;
            return (static_cast<std::uint32_t>(b0 & 0x07U) << 18U) |
                (static_cast<std::uint32_t>(b1 & 0x3FU) << 12U) |
                (static_cast<std::uint32_t>(b2 & 0x3FU) << 6U) |
                static_cast<std::uint32_t>(b3 & 0x3FU);
        }
    }
    ++offset;
    return '?';
}

// Slot 0 is reserved for a solid 255 white tile.
// Slots 1..94 map ASCII 0x21..0x7E.
// Slots 95..126 map Cyrillic А..Я (U+0410..U+042F).
// Slots 127..158 map Cyrillic а..я (U+0430..U+044F).
// Slot 159 maps Ё (U+0401), Slot 160 maps ё (U+0451).
// Slot 161 is '?' fallback.
constexpr std::uint32_t slot_for_codepoint(const std::uint32_t cp) noexcept {
    if (cp >= 0x21U && cp <= 0x7EU) {
        return cp - 0x20U;
    }
    if (cp >= 0x0410U && cp <= 0x042FU) {
        return 95U + (cp - 0x0410U);
    }
    if (cp >= 0x0430U && cp <= 0x044FU) {
        return 127U + (cp - 0x0430U);
    }
    if (cp == 0x0401U) {
        return 159U;
    }
    if (cp == 0x0451U) {
        return 160U;
    }
    return 0x3FU - 0x20U; // '?'
}

constexpr std::uint32_t total_glyph_slots = 161U;

constexpr std::array<GlyphBitmap, 94U> kAsciiGlyphs{{
    {{0x04,0x04,0x04,0x04,0x00,0x00,0x04}}, // '!'
    {{0x0A,0x0A,0x00,0x00,0x00,0x00,0x00}}, // '"'
    {{0x0A,0x0A,0x1F,0x0A,0x1F,0x0A,0x0A}}, // '#'
    {{0x04,0x0F,0x14,0x0E,0x05,0x1E,0x04}}, // '$'
    {{0x18,0x19,0x02,0x04,0x08,0x13,0x03}}, // '%'
    {{0x0C,0x12,0x14,0x08,0x15,0x12,0x0D}}, // '&'
    {{0x0C,0x04,0x08,0x00,0x00,0x00,0x00}}, // '''
    {{0x02,0x04,0x08,0x08,0x08,0x04,0x02}}, // '('
    {{0x08,0x04,0x02,0x02,0x02,0x04,0x08}}, // ')'
    {{0x00,0x04,0x15,0x0E,0x15,0x04,0x00}}, // '*'
    {{0x00,0x04,0x04,0x1F,0x04,0x04,0x00}}, // '+'
    {{0x00,0x00,0x00,0x00,0x0C,0x04,0x08}}, // ','
    {{0x00,0x00,0x00,0x1F,0x00,0x00,0x00}}, // '-'
    {{0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}}, // '.'
    {{0x01,0x01,0x02,0x04,0x08,0x10,0x10}}, // '/'
    {{0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}}, // '0'
    {{0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}}, // '1'
    {{0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}}, // '2'
    {{0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}}, // '3'
    {{0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}}, // '4'
    {{0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}}, // '5'
    {{0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}}, // '6'
    {{0x1F,0x01,0x02,0x04,0x08,0x08,0x08}}, // '7'
    {{0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}}, // '8'
    {{0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}}, // '9'
    {{0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00}}, // ':'
    {{0x00,0x0C,0x0C,0x00,0x0C,0x04,0x08}}, // ';'
    {{0x02,0x04,0x08,0x10,0x08,0x04,0x02}}, // '<'
    {{0x00,0x00,0x1F,0x00,0x1F,0x00,0x00}}, // '='
    {{0x08,0x04,0x02,0x01,0x02,0x04,0x08}}, // '>'
    {{0x0E,0x11,0x01,0x02,0x04,0x00,0x04}}, // '?'
    {{0x0E,0x11,0x01,0x0D,0x15,0x15,0x0E}}, // '@'
    {{0x0E,0x11,0x11,0x11,0x1F,0x11,0x11}}, // 'A'
    {{0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}}, // 'B'
    {{0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}}, // 'C'
    {{0x1C,0x12,0x11,0x11,0x11,0x12,0x1C}}, // 'D'
    {{0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}}, // 'E'
    {{0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}}, // 'F'
    {{0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}}, // 'G'
    {{0x11,0x11,0x11,0x1F,0x11,0x11,0x11}}, // 'H'
    {{0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}}, // 'I'
    {{0x07,0x02,0x02,0x02,0x02,0x12,0x0C}}, // 'J'
    {{0x11,0x12,0x14,0x18,0x14,0x12,0x11}}, // 'K'
    {{0x10,0x10,0x10,0x10,0x10,0x10,0x1F}}, // 'L'
    {{0x11,0x1B,0x15,0x15,0x11,0x11,0x11}}, // 'M'
    {{0x11,0x11,0x19,0x15,0x13,0x11,0x11}}, // 'N'
    {{0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}}, // 'O'
    {{0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}}, // 'P'
    {{0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}}, // 'Q'
    {{0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}}, // 'R'
    {{0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}}, // 'S'
    {{0x1F,0x04,0x04,0x04,0x04,0x04,0x04}}, // 'T'
    {{0x11,0x11,0x11,0x11,0x11,0x11,0x0E}}, // 'U'
    {{0x11,0x11,0x11,0x11,0x11,0x0A,0x04}}, // 'V'
    {{0x11,0x11,0x11,0x15,0x15,0x1B,0x11}}, // 'W'
    {{0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}}, // 'X'
    {{0x11,0x11,0x11,0x0A,0x04,0x04,0x04}}, // 'Y'
    {{0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}}, // 'Z'
    {{0x0E,0x08,0x08,0x08,0x08,0x08,0x0E}}, // '['
    {{0x10,0x10,0x08,0x04,0x02,0x01,0x01}}, // '\'
    {{0x0E,0x02,0x02,0x02,0x02,0x02,0x0E}}, // ']'
    {{0x04,0x0A,0x11,0x00,0x00,0x00,0x00}}, // '^'
    {{0x00,0x00,0x00,0x00,0x00,0x00,0x1F}}, // '_'
    {{0x08,0x04,0x02,0x00,0x00,0x00,0x00}}, // '`'
    {{0x00,0x00,0x0E,0x01,0x0F,0x11,0x0F}}, // 'a'
    {{0x10,0x10,0x16,0x19,0x11,0x11,0x1E}}, // 'b'
    {{0x00,0x00,0x0E,0x10,0x10,0x11,0x0E}}, // 'c'
    {{0x01,0x01,0x0D,0x13,0x11,0x11,0x0F}}, // 'd'
    {{0x00,0x00,0x0E,0x11,0x1F,0x10,0x0E}}, // 'e'
    {{0x06,0x09,0x08,0x1C,0x08,0x08,0x08}}, // 'f'
    {{0x00,0x0F,0x11,0x11,0x0F,0x01,0x0E}}, // 'g'
    {{0x10,0x10,0x16,0x19,0x11,0x11,0x11}}, // 'h'
    {{0x04,0x00,0x0C,0x04,0x04,0x04,0x0E}}, // 'i'
    {{0x02,0x00,0x06,0x02,0x02,0x12,0x0C}}, // 'j'
    {{0x10,0x10,0x12,0x14,0x18,0x14,0x12}}, // 'k'
    {{0x0C,0x04,0x04,0x04,0x04,0x04,0x0E}}, // 'l'
    {{0x00,0x00,0x1A,0x15,0x15,0x11,0x11}}, // 'm'
    {{0x00,0x00,0x16,0x19,0x11,0x11,0x11}}, // 'n'
    {{0x00,0x00,0x0E,0x11,0x11,0x11,0x0E}}, // 'o'
    {{0x00,0x00,0x1E,0x11,0x1E,0x10,0x10}}, // 'p'
    {{0x00,0x00,0x0D,0x13,0x0F,0x01,0x01}}, // 'q'
    {{0x00,0x00,0x16,0x19,0x10,0x10,0x10}}, // 'r'
    {{0x00,0x00,0x0E,0x10,0x0E,0x01,0x1E}}, // 's'
    {{0x08,0x08,0x1C,0x08,0x08,0x09,0x06}}, // 't'
    {{0x00,0x00,0x11,0x11,0x11,0x13,0x0D}}, // 'u'
    {{0x00,0x00,0x11,0x11,0x11,0x0A,0x04}}, // 'v'
    {{0x00,0x00,0x11,0x11,0x15,0x15,0x0A}}, // 'w'
    {{0x00,0x00,0x11,0x0A,0x04,0x0A,0x11}}, // 'x'
    {{0x00,0x00,0x11,0x11,0x0F,0x01,0x0E}}, // 'y'
    {{0x00,0x00,0x1F,0x02,0x04,0x08,0x1F}}, // 'z'
    {{0x02,0x04,0x04,0x08,0x04,0x04,0x02}}, // '{'
    {{0x04,0x04,0x04,0x00,0x04,0x04,0x04}}, // '|'
    {{0x08,0x04,0x04,0x02,0x04,0x04,0x08}}, // '}'
    {{0x00,0x00,0x08,0x15,0x02,0x00,0x00}}, // '~'
}};

// А..Я (32) + а..я (32) + Ё (1) + ё (1) = 66 Cyrillic glyphs
constexpr std::array<GlyphBitmap, 66U> kCyrillicGlyphs{{
    // Uppercase А..Я (0..31)
    {{0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}}, // А
    {{0x1F,0x10,0x10,0x1E,0x11,0x11,0x1E}}, // Б
    {{0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}}, // В
    {{0x1F,0x10,0x10,0x10,0x10,0x10,0x10}}, // Г
    {{0x06,0x0A,0x0A,0x0A,0x0A,0x1F,0x11}}, // Д
    {{0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}}, // Е
    {{0x15,0x15,0x0E,0x04,0x0E,0x15,0x15}}, // Ж
    {{0x0E,0x11,0x01,0x06,0x01,0x11,0x0E}}, // З
    {{0x11,0x11,0x13,0x15,0x19,0x11,0x11}}, // И
    {{0x0A,0x04,0x11,0x13,0x15,0x19,0x11}}, // Й
    {{0x11,0x12,0x14,0x18,0x14,0x12,0x11}}, // К
    {{0x07,0x09,0x09,0x09,0x09,0x09,0x11}}, // Л
    {{0x11,0x1B,0x15,0x15,0x11,0x11,0x11}}, // М
    {{0x11,0x11,0x11,0x1F,0x11,0x11,0x11}}, // Н
    {{0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}}, // О
    {{0x1F,0x11,0x11,0x11,0x11,0x11,0x11}}, // П
    {{0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}}, // Р
    {{0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}}, // С
    {{0x1F,0x04,0x04,0x04,0x04,0x04,0x04}}, // Т
    {{0x11,0x11,0x11,0x0F,0x01,0x11,0x0E}}, // У
    {{0x04,0x0E,0x15,0x15,0x0E,0x04,0x04}}, // Ф
    {{0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}}, // Х
    {{0x12,0x12,0x12,0x12,0x12,0x1F,0x01}}, // Ц
    {{0x11,0x11,0x11,0x0F,0x01,0x01,0x01}}, // Ч
    {{0x15,0x15,0x15,0x15,0x15,0x15,0x1F}}, // Ш
    {{0x15,0x15,0x15,0x15,0x15,0x1F,0x01}}, // Щ
    {{0x18,0x08,0x08,0x0E,0x09,0x09,0x0E}}, // Ъ
    {{0x11,0x11,0x11,0x1D,0x15,0x15,0x1D}}, // Ы
    {{0x10,0x10,0x10,0x1E,0x11,0x11,0x1E}}, // Ь
    {{0x0E,0x11,0x01,0x07,0x01,0x11,0x0E}}, // Э
    {{0x12,0x15,0x15,0x1D,0x15,0x15,0x12}}, // Ю
    {{0x0F,0x11,0x11,0x0F,0x05,0x09,0x11}}, // Я
    // Lowercase а..я (32..63)
    {{0x00,0x00,0x0E,0x01,0x0F,0x11,0x0F}}, // а
    {{0x02,0x0C,0x10,0x1E,0x11,0x11,0x0E}}, // б
    {{0x00,0x00,0x1E,0x11,0x1E,0x11,0x1E}}, // в
    {{0x00,0x00,0x1F,0x10,0x10,0x10,0x10}}, // г
    {{0x00,0x0E,0x0A,0x0A,0x0A,0x1F,0x11}}, // д
    {{0x00,0x00,0x0E,0x11,0x1F,0x10,0x0E}}, // е
    {{0x00,0x00,0x15,0x0E,0x04,0x0E,0x15}}, // ж
    {{0x00,0x00,0x0E,0x01,0x06,0x01,0x0E}}, // з
    {{0x00,0x00,0x11,0x13,0x15,0x19,0x11}}, // и
    {{0x04,0x00,0x11,0x13,0x15,0x19,0x11}}, // й
    {{0x00,0x00,0x11,0x12,0x1C,0x12,0x11}}, // к
    {{0x00,0x00,0x07,0x09,0x09,0x09,0x11}}, // л
    {{0x00,0x00,0x11,0x1B,0x15,0x11,0x11}}, // м
    {{0x00,0x00,0x11,0x11,0x1F,0x11,0x11}}, // н
    {{0x00,0x00,0x0E,0x11,0x11,0x11,0x0E}}, // о
    {{0x00,0x00,0x1F,0x11,0x11,0x11,0x11}}, // п
    {{0x00,0x00,0x1E,0x11,0x1E,0x10,0x10}}, // р
    {{0x00,0x00,0x0E,0x10,0x10,0x10,0x0E}}, // с
    {{0x00,0x00,0x1F,0x04,0x04,0x04,0x04}}, // т
    {{0x00,0x00,0x11,0x11,0x0F,0x01,0x0E}}, // у
    {{0x04,0x0E,0x15,0x15,0x0E,0x04,0x04}}, // ф
    {{0x00,0x00,0x11,0x0A,0x04,0x0A,0x11}}, // х
    {{0x00,0x00,0x12,0x12,0x12,0x1F,0x01}}, // ц
    {{0x00,0x00,0x11,0x11,0x0F,0x01,0x01}}, // ч
    {{0x00,0x00,0x15,0x15,0x15,0x15,0x1F}}, // ш
    {{0x00,0x00,0x15,0x15,0x15,0x1F,0x01}}, // щ
    {{0x00,0x00,0x18,0x08,0x0E,0x09,0x0E}}, // ъ
    {{0x00,0x00,0x11,0x11,0x1D,0x15,0x1D}}, // ы
    {{0x00,0x00,0x10,0x10,0x1E,0x11,0x1E}}, // ь
    {{0x00,0x00,0x0E,0x01,0x07,0x01,0x0E}}, // э
    {{0x00,0x00,0x12,0x15,0x1D,0x15,0x12}}, // ю
    {{0x00,0x00,0x0F,0x11,0x0F,0x05,0x11}}, // я
    // Ё, ё (64..65)
    {{0x0A,0x1F,0x10,0x1E,0x10,0x10,0x1F}}, // Ё
    {{0x0A,0x00,0x0E,0x11,0x1F,0x10,0x0E}}, // ё
}};

constexpr GlyphBitmap bitmap_for_slot(const std::uint32_t slot) noexcept {
    if (slot >= 1U && slot <= 94U) {
        return kAsciiGlyphs[slot - 1U];
    }
    if (slot >= 95U && slot < 161U) {
        return kCyrillicGlyphs[slot - 95U];
    }
    return GlyphBitmap{};
}

glm::vec2 solid_white_uv() noexcept {
    constexpr float center = 8.0F / static_cast<float>(game_ui_atlas_size);
    return glm::vec2(center, center);
}

void slot_uv_bounds(const std::uint32_t slot, glm::vec2& out_min, glm::vec2& out_max) noexcept {
    const std::uint32_t col = slot % game_ui_glyph_grid_dim;
    const std::uint32_t row = slot / game_ui_glyph_grid_dim;
    constexpr float inv_atlas = 1.0F / static_cast<float>(game_ui_atlas_size);
    const float x0 = static_cast<float>(col * game_ui_glyph_cell_size + 3U);
    const float y0 = static_cast<float>(row * game_ui_glyph_cell_size + 1U);
    out_min = glm::vec2(x0 * inv_atlas, y0 * inv_atlas);
    out_max = glm::vec2((x0 + 10.0F) * inv_atlas, (y0 + 14.0F) * inv_atlas);
}

struct StbiDeleter {
    void operator()(stbi_uc* ptr) const noexcept {
        stbi_image_free(ptr);
    }
};
using StbiPixels = std::unique_ptr<stbi_uc, StbiDeleter>;

struct VecSegment {
    glm::vec2 a{};
    glm::vec2 b{};
};

[[nodiscard]] float sd_segment(const glm::vec2 p, const glm::vec2 a, const glm::vec2 b) noexcept {
    const glm::vec2 pa = p - a;
    const glm::vec2 ba = b - a;
    const float denom = glm::dot(ba, ba);
    const float h = denom > 1.0e-6F ? std::clamp(glm::dot(pa, ba) / denom, 0.0F, 1.0F) : 0.0F;
    return glm::length(pa - ba * h);
}

void rasterize_segments_sdf(
    std::vector<std::uint8_t>& pixels,
    const std::uint32_t atlas_size,
    const std::uint32_t cell_size,
    const std::uint32_t grid_dim,
    const std::uint32_t slot,
    const std::span<const VecSegment> segments,
    const float stroke_radius_px,
    const float spread_px) noexcept {
    const std::uint32_t cell_x = (slot % grid_dim) * cell_size;
    const std::uint32_t cell_y = (slot / grid_dim) * cell_size;
    for (std::uint32_t py = 0U; py < cell_size; ++py) {
        for (std::uint32_t px = 0U; px < cell_size; ++px) {
            const glm::vec2 sample(static_cast<float>(px) + 0.5F, static_cast<float>(py) + 0.5F);
            float min_d = 1.0e6F;
            for (const auto& seg : segments) {
                min_d = std::min(min_d, sd_segment(sample, seg.a, seg.b));
            }
            const float encoded = std::clamp(0.5F + (stroke_radius_px - min_d) / spread_px, 0.0F, 1.0F);
            pixels[(cell_y + py) * atlas_size + (cell_x + px)] =
                static_cast<std::uint8_t>(std::lround(encoded * 255.0F));
        }
    }
}

void rasterize_bitmap_glyph_sdf(
    std::vector<std::uint8_t>& pixels,
    const std::uint32_t atlas_size,
    const std::uint32_t cell_size,
    const std::uint32_t grid_dim,
    const std::uint32_t slot,
    const GlyphBitmap& glyph,
    float& out_left_px,
    float& out_right_px) noexcept {
    constexpr float origin_x = 9.0F;
    constexpr float origin_y = 6.2F;
    constexpr float step_x = 3.2F;
    constexpr float step_y = 3.15F;
    constexpr float stroke_radius = 1.65F;
    constexpr float spread = 3.30F;

    auto on = [&](const int c, const int r) -> bool {
        if (c < 0 || c >= 5 || r < 0 || r >= 7) {
            return false;
        }
        return ((glyph.rows[static_cast<std::size_t>(r)] >> (4 - c)) & 1U) != 0U;
    };
    auto pt = [&](const int c, const int r) -> glm::vec2 {
        return glm::vec2(origin_x + static_cast<float>(c) * step_x, origin_y + static_cast<float>(r) * step_y);
    };

    std::array<VecSegment, 64U> segments{};
    std::size_t seg_count = 0U;
    int min_c = 4;
    int max_c = 0;
    bool any_on = false;

    for (int r = 0; r < 7; ++r) {
        for (int c = 0; c < 5; ++c) {
            if (!on(c, r)) {
                continue;
            }
            any_on = true;
            min_c = std::min(min_c, c);
            max_c = std::max(max_c, c);

            bool connected = false;
            if (on(c + 1, r) && seg_count < segments.size()) {
                segments[seg_count++] = VecSegment{pt(c, r), pt(c + 1, r)};
                connected = true;
            }
            if (on(c, r + 1) && seg_count < segments.size()) {
                segments[seg_count++] = VecSegment{pt(c, r), pt(c, r + 1)};
                connected = true;
            }
            if (on(c + 1, r + 1) && !on(c + 1, r) && !on(c, r + 1) && seg_count < segments.size()) {
                segments[seg_count++] = VecSegment{pt(c, r), pt(c + 1, r + 1)};
                connected = true;
            }
            if (on(c - 1, r + 1) && !on(c - 1, r) && !on(c, r + 1) && seg_count < segments.size()) {
                segments[seg_count++] = VecSegment{pt(c, r), pt(c - 1, r + 1)};
                connected = true;
            }
            if (!connected && seg_count < segments.size()) {
                // Keep isolated dots/caps crisp without zero-length duplicate segments when connected
                const bool has_prev = on(c - 1, r) || on(c, r - 1) ||
                    (on(c - 1, r - 1) && !on(c - 1, r) && !on(c, r - 1)) ||
                    (on(c + 1, r - 1) && !on(c + 1, r) && !on(c, r - 1));
                if (!has_prev) {
                    segments[seg_count++] = VecSegment{pt(c, r), pt(c, r)};
                }
            }
        }
    }

    if (!any_on) {
        out_left_px = 9.0F;
        out_right_px = 14.0F;
        return;
    }

    out_left_px = origin_x + static_cast<float>(min_c) * step_x - stroke_radius;
    out_right_px = origin_x + static_cast<float>(max_c) * step_x + stroke_radius;
    rasterize_segments_sdf(
        pixels,
        atlas_size,
        cell_size,
        grid_dim,
        slot,
        std::span<const VecSegment>{segments.data(), seg_count},
        stroke_radius,
        spread);
}

struct ExtraVecGlyphSpec {
    std::uint32_t codepoint;
    float advance_px;
    std::array<VecSegment, 6U> segments;
    std::size_t segment_count;
};

constexpr std::array<ExtraVecGlyphSpec, 13U> kExtraVectorGlyphs{{
    // « »
    {0x00ABU, 16.0F, {{{{12.5F, 11.0F}, {8.5F, 16.0F}}, {{8.5F, 16.0F}, {12.5F, 21.0F}}, {{18.5F, 11.0F}, {14.5F, 16.0F}}, {{14.5F, 16.0F}, {18.5F, 21.0F}}}}, 4U},
    {0x00BBU, 16.0F, {{{{9.5F, 11.0F}, {13.5F, 16.0F}}, {{13.5F, 16.0F}, {9.5F, 21.0F}}, {{15.5F, 11.0F}, {19.5F, 16.0F}}, {{19.5F, 16.0F}, {15.5F, 21.0F}}}}, 4U},
    // — –
    {0x2014U, 20.0F, {{{{7.0F, 16.0F}, {23.0F, 16.0F}}}}, 1U},
    {0x2013U, 15.0F, {{{{9.0F, 16.0F}, {19.0F, 16.0F}}}}, 1U},
    // …
    {0x2026U, 16.0F, {{{{9.0F, 24.5F}, {9.0F, 24.5F}}, {{14.5F, 24.5F}, {14.5F, 24.5F}}, {{20.0F, 24.5F}, {20.0F, 24.5F}}}}, 3U},
    // №
    {0x2116U, 22.0F, {{{{7.5F, 25.0F}, {7.5F, 6.5F}}, {{7.5F, 6.5F}, {15.0F, 25.0F}}, {{15.0F, 25.0F}, {15.0F, 6.5F}}, {{20.5F, 10.5F}, {20.5F, 13.5F}}, {{18.5F, 17.5F}, {22.5F, 17.5F}}}}, 5U},
    // °
    {0x00B0U, 11.0F, {{{{11.5F, 7.5F}, {13.5F, 9.5F}}, {{13.5F, 9.5F}, {11.5F, 11.5F}}, {{11.5F, 11.5F}, {9.5F, 9.5F}}, {{9.5F, 9.5F}, {11.5F, 7.5F}}}}, 4U},
    // •
    {0x2022U, 13.0F, {{{{14.0F, 15.5F}, {14.0F, 15.5F}}}}, 1U},
    // ← → ↑ ↓
    {0x2190U, 19.0F, {{{{21.5F, 16.0F}, {8.5F, 16.0F}}, {{8.5F, 16.0F}, {13.5F, 11.0F}}, {{8.5F, 16.0F}, {13.5F, 21.0F}}}}, 3U},
    {0x2192U, 19.0F, {{{{8.5F, 16.0F}, {21.5F, 16.0F}}, {{21.5F, 16.0F}, {16.5F, 11.0F}}, {{21.5F, 16.0F}, {16.5F, 21.0F}}}}, 3U},
    {0x2191U, 16.0F, {{{{14.5F, 24.5F}, {14.5F, 7.5F}}, {{14.5F, 7.5F}, {9.5F, 12.5F}}, {{14.5F, 7.5F}, {19.5F, 12.5F}}}}, 3U},
    {0x2193U, 16.0F, {{{{14.5F, 7.5F}, {14.5F, 24.5F}}, {{14.5F, 24.5F}, {9.5F, 19.5F}}, {{14.5F, 24.5F}, {19.5F, 19.5F}}}}, 3U},
    // ✓
    {0x2713U, 18.0F, {{{{8.0F, 16.5F}, {13.0F, 22.5F}}, {{13.0F, 22.5F}, {22.0F, 9.5F}}}}, 2U},
}};

[[nodiscard]] std::uint32_t codepoint_for_slot(const std::uint32_t slot) noexcept {
    if (slot >= 1U && slot <= 94U) {
        return 0x20U + slot;
    }
    if (slot >= 95U && slot <= 126U) {
        return 0x0410U + (slot - 95U);
    }
    if (slot >= 127U && slot <= 158U) {
        return 0x0430U + (slot - 127U);
    }
    if (slot == 159U) {
        return 0x0401U;
    }
    if (slot == 160U) {
        return 0x0451U;
    }
    return '?';
}

} // namespace

void GameUiFontAtlas::rebuild_lookup() noexcept {
    ascii_lookup_.fill(-1);
    question_index_ = -1;
    for (std::size_t i = 0U; i < glyphs.size(); ++i) {
        const std::uint32_t cp = glyphs[i].codepoint;
        if (cp < ascii_lookup_.size() && i <= 32767U) {
            ascii_lookup_[cp] = static_cast<std::int16_t>(i);
        }
        if (cp == '?' && i <= 32767U) {
            question_index_ = static_cast<std::int16_t>(i);
        }
    }
}

const GameUiGlyphMetrics* GameUiFontAtlas::find_glyph(const std::uint32_t codepoint) const noexcept {
    if (codepoint < ascii_lookup_.size()) {
        const std::int16_t idx = ascii_lookup_[codepoint];
        return idx >= 0 ? &glyphs[static_cast<std::size_t>(idx)] : nullptr;
    }
    const auto it = std::lower_bound(
        glyphs.begin(),
        glyphs.end(),
        codepoint,
        [](const GameUiGlyphMetrics& lhs, const std::uint32_t rhs) { return lhs.codepoint < rhs; });
    if (it != glyphs.end() && it->codepoint == codepoint) {
        return &(*it);
    }
    return nullptr;
}

const GameUiGlyphMetrics& GameUiFontAtlas::fallback_glyph() const noexcept {
    static constexpr GameUiGlyphMetrics kEmptyFallback{};
    if (question_index_ >= 0 && static_cast<std::size_t>(question_index_) < glyphs.size()) {
        return glyphs[static_cast<std::size_t>(question_index_)];
    }
    return glyphs.empty() ? kEmptyFallback : glyphs.front();
}

GameUiFontAtlas GameUiFontAtlas::build_default_sdf() {
    GameUiFontAtlas atlas{};
    atlas.width = game_ui_sdf_atlas_size;
    atlas.height = game_ui_sdf_atlas_size;
    atlas.base_pixel_size = 20.0F;
    atlas.cap_height_px = 22.2F;
    atlas.line_height_px = 27.5F;
    atlas.sdf = true;
    atlas.sdf_spread_px = 3.30F;
    atlas.pixels_r8.assign(
        static_cast<std::size_t>(atlas.width) * static_cast<std::size_t>(atlas.height), 0U);

    // Slot 0: solid white block for rects/outlines/lines/arcs/crosshairs.
    for (std::uint32_t py = 0U; py < game_ui_sdf_cell_size; ++py) {
        for (std::uint32_t px = 0U; px < game_ui_sdf_cell_size; ++px) {
            atlas.pixels_r8[py * atlas.width + px] = 255U;
        }
    }
    constexpr float inv_atlas = 1.0F / static_cast<float>(game_ui_sdf_atlas_size);
    atlas.solid_uv_min = glm::vec2(4.0F * inv_atlas, 4.0F * inv_atlas);
    atlas.solid_uv_max = glm::vec2(28.0F * inv_atlas, 28.0F * inv_atlas);

    atlas.glyphs.reserve(176U);
    atlas.glyphs.push_back(GameUiGlyphMetrics{
        .codepoint = ' ',
        .uv_min = atlas.solid_uv_min,
        .uv_max = atlas.solid_uv_min,
        .size_px = glm::vec2(0.0F, 0.0F),
        .bearing_px = glm::vec2(0.0F, 0.0F),
        .advance_px = 9.0F,
    });

    for (std::uint32_t slot = 1U; slot < total_glyph_slots; ++slot) {
        const GlyphBitmap glyph = bitmap_for_slot(slot);
        float left_px = 9.0F;
        float right_px = 18.0F;
        rasterize_bitmap_glyph_sdf(
            atlas.pixels_r8,
            atlas.width,
            game_ui_sdf_cell_size,
            game_ui_sdf_grid_dim,
            slot,
            glyph,
            left_px,
            right_px);

        const float cell_x = static_cast<float>((slot % game_ui_sdf_grid_dim) * game_ui_sdf_cell_size);
        const float cell_y = static_cast<float>((slot / game_ui_sdf_grid_dim) * game_ui_sdf_cell_size);
        const glm::vec2 uv_min((cell_x + 4.0F) * inv_atlas, (cell_y + 2.0F) * inv_atlas);
        const glm::vec2 uv_max((cell_x + 28.0F) * inv_atlas, (cell_y + 30.0F) * inv_atlas);
        const float ink_w = std::max(right_px - left_px, 4.5F);
        atlas.glyphs.push_back(GameUiGlyphMetrics{
            .codepoint = codepoint_for_slot(slot),
            .uv_min = uv_min,
            .uv_max = uv_max,
            .size_px = glm::vec2(24.0F, 28.0F),
            .bearing_px = glm::vec2(-(left_px - 4.0F) + 0.5F, -2.55F),
            .advance_px = std::max(ink_w + 3.6F, 8.2F),
        });
    }

    for (std::size_t i = 0U; i < kExtraVectorGlyphs.size(); ++i) {
        const std::uint32_t slot = static_cast<std::uint32_t>(total_glyph_slots + i);
        const auto& spec = kExtraVectorGlyphs[i];
        rasterize_segments_sdf(
            atlas.pixels_r8,
            atlas.width,
            game_ui_sdf_cell_size,
            game_ui_sdf_grid_dim,
            slot,
            std::span<const VecSegment>{spec.segments.data(), spec.segment_count},
            1.65F,
            3.30F);

        const float cell_x = static_cast<float>((slot % game_ui_sdf_grid_dim) * game_ui_sdf_cell_size);
        const float cell_y = static_cast<float>((slot / game_ui_sdf_grid_dim) * game_ui_sdf_cell_size);
        atlas.glyphs.push_back(GameUiGlyphMetrics{
            .codepoint = spec.codepoint,
            .uv_min = glm::vec2((cell_x + 4.0F) * inv_atlas, (cell_y + 2.0F) * inv_atlas),
            .uv_max = glm::vec2((cell_x + 28.0F) * inv_atlas, (cell_y + 30.0F) * inv_atlas),
            .size_px = glm::vec2(24.0F, 28.0F),
            .bearing_px = glm::vec2(-3.0F, -2.55F),
            .advance_px = spec.advance_px,
        });
    }

    std::sort(
        atlas.glyphs.begin(),
        atlas.glyphs.end(),
        [](const GameUiGlyphMetrics& lhs, const GameUiGlyphMetrics& rhs) {
            return lhs.codepoint < rhs.codepoint;
        });
    atlas.rebuild_lookup();
    return atlas;
}

GameUiFontAtlas GameUiFontAtlas::build_bitmap_fallback() {
    GameUiFontAtlas atlas{};
    atlas.width = game_ui_atlas_size;
    atlas.height = game_ui_atlas_size;
    atlas.base_pixel_size = 14.0F;
    atlas.cap_height_px = 14.0F;
    atlas.line_height_px = 16.0F;
    atlas.sdf = false;
    atlas.sdf_spread_px = 0.0F;
    atlas.pixels_r8.resize(static_cast<std::size_t>(game_ui_atlas_size) * game_ui_atlas_size);
    std::array<std::uint8_t, game_ui_atlas_size * game_ui_atlas_size> legacy_pixels{};
    build_game_ui_font_atlas(legacy_pixels);
    std::copy(legacy_pixels.begin(), legacy_pixels.end(), atlas.pixels_r8.begin());

    constexpr float inv_atlas = 1.0F / static_cast<float>(game_ui_atlas_size);
    atlas.solid_uv_min = glm::vec2(4.0F * inv_atlas, 4.0F * inv_atlas);
    atlas.solid_uv_max = glm::vec2(12.0F * inv_atlas, 12.0F * inv_atlas);

    atlas.glyphs.reserve(164U);
    atlas.glyphs.push_back(GameUiGlyphMetrics{
        .codepoint = ' ',
        .uv_min = atlas.solid_uv_min,
        .uv_max = atlas.solid_uv_min,
        .size_px = glm::vec2(0.0F, 0.0F),
        .bearing_px = glm::vec2(0.0F, 0.0F),
        .advance_px = 12.0F,
    });
    for (std::uint32_t slot = 1U; slot < total_glyph_slots; ++slot) {
        glm::vec2 uv_min{};
        glm::vec2 uv_max{};
        slot_uv_bounds(slot, uv_min, uv_max);
        atlas.glyphs.push_back(GameUiGlyphMetrics{
            .codepoint = codepoint_for_slot(slot),
            .uv_min = uv_min,
            .uv_max = uv_max,
            .size_px = glm::vec2(10.0F, 14.0F),
            .bearing_px = glm::vec2(0.0F, 0.0F),
            .advance_px = 12.0F,
        });
    }
    std::sort(
        atlas.glyphs.begin(),
        atlas.glyphs.end(),
        [](const GameUiGlyphMetrics& lhs, const GameUiGlyphMetrics& rhs) {
            return lhs.codepoint < rhs.codepoint;
        });
    atlas.rebuild_lookup();
    return atlas;
}

const GameUiFontAtlas& default_game_ui_font_atlas() {
    static const GameUiFontAtlas kDefaultSdf = GameUiFontAtlas::build_default_sdf();
    return kDefaultSdf;
}

bool save_game_ui_font_atlas_file(
    const std::filesystem::path& json_path,
    const GameUiFontAtlas& atlas,
    std::string* error_message) {
    if (atlas.width == 0U || atlas.height == 0U ||
        atlas.pixels_r8.size() != static_cast<std::size_t>(atlas.width) * static_cast<std::size_t>(atlas.height)) {
        if (error_message != nullptr) {
            *error_message = "Font atlas dimensions do not match pixel buffer size.";
        }
        return false;
    }

    std::error_code ec;
    if (json_path.has_parent_path()) {
        std::filesystem::create_directories(json_path.parent_path(), ec);
    }

    std::filesystem::path png_path = json_path;
    const std::string filename = json_path.filename().string();
    constexpr std::string_view suffix = ".vcrfont.json";
    if (filename.ends_with(suffix)) {
        const std::string base = filename.substr(0U, filename.size() - suffix.size());
        png_path.replace_filename(base + ".png");
    } else {
        png_path.replace_extension(".png");
    }

    std::vector<std::uint8_t> rgba(atlas.pixels_r8.size() * 4U);
    for (std::size_t i = 0U; i < atlas.pixels_r8.size(); ++i) {
        const std::uint8_t v = atlas.pixels_r8[i];
        rgba[i * 4U + 0U] = v;
        rgba[i * 4U + 1U] = v;
        rgba[i * 4U + 2U] = v;
        rgba[i * 4U + 3U] = 255U;
    }
    const PngEncodeResult encoded = encode_png_rgba(rgba, atlas.width, atlas.height);
    if (!encoded) {
        if (error_message != nullptr) {
            *error_message = encoded.error;
        }
        return false;
    }
    std::ofstream png_out(png_path, std::ios::binary | std::ios::trunc);
    if (!png_out.is_open()) {
        if (error_message != nullptr) {
            *error_message = "Failed to open font atlas PNG for write: " + png_path.string();
        }
        return false;
    }
    png_out.write(
        reinterpret_cast<const char*>(encoded.bytes.data()),
        static_cast<std::streamsize>(encoded.bytes.size()));
    if (!png_out.good()) {
        if (error_message != nullptr) {
            *error_message = "Failed writing font atlas PNG: " + png_path.string();
        }
        return false;
    }

    nlohmann::json glyph_array = nlohmann::json::array();
    for (const auto& g : atlas.glyphs) {
        glyph_array.push_back(nlohmann::json{
            {"codepoint", g.codepoint},
            {"uv_min", {g.uv_min.x, g.uv_min.y}},
            {"uv_max", {g.uv_max.x, g.uv_max.y}},
            {"size_px", {g.size_px.x, g.size_px.y}},
            {"bearing_px", {g.bearing_px.x, g.bearing_px.y}},
            {"advance_px", g.advance_px},
        });
    }

    const nlohmann::json root{
        {"format", "vcr_font_atlas_v1"},
        {"image", png_path.filename().string()},
        {"width", atlas.width},
        {"height", atlas.height},
        {"base_pixel_size", atlas.base_pixel_size},
        {"cap_height_px", atlas.cap_height_px},
        {"line_height_px", atlas.line_height_px},
        {"sdf", atlas.sdf},
        {"sdf_spread_px", atlas.sdf_spread_px},
        {"solid_uv_min", {atlas.solid_uv_min.x, atlas.solid_uv_min.y}},
        {"solid_uv_max", {atlas.solid_uv_max.x, atlas.solid_uv_max.y}},
        {"glyphs", std::move(glyph_array)},
    };

    std::ofstream out(json_path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        if (error_message != nullptr) {
            *error_message = "Failed to open font JSON for write: " + json_path.string();
        }
        return false;
    }
    out << root.dump(2) << '\n';
    if (!out.good()) {
        if (error_message != nullptr) {
            *error_message = "Failed writing font JSON: " + json_path.string();
        }
        return false;
    }
    return true;
}

bool load_game_ui_font_atlas_file(
    const std::filesystem::path& json_path,
    GameUiFontAtlas& out_atlas,
    std::string* error_message) {
    std::ifstream in(json_path, std::ios::binary);
    if (!in.is_open()) {
        if (error_message != nullptr) {
            *error_message = "Failed to open font JSON: " + json_path.string();
        }
        return false;
    }

    nlohmann::json root;
    try {
        in >> root;
    } catch (const std::exception& e) {
        if (error_message != nullptr) {
            *error_message = "Failed to parse font JSON '" + json_path.string() + "': " + e.what();
        }
        return false;
    }

    if (!root.is_object() || !root.contains("image") || !root["image"].is_string()) {
        if (error_message != nullptr) {
            *error_message = "Font JSON missing valid 'image' path: " + json_path.string();
        }
        return false;
    }

    const std::filesystem::path image_rel = root.value("image", std::string{});
    const std::filesystem::path image_path =
        image_rel.is_absolute() ? image_rel : (json_path.parent_path() / image_rel);

    int img_w = 0;
    int img_h = 0;
    int img_comp = 0;
    const StbiPixels loaded(stbi_load(image_path.string().c_str(), &img_w, &img_h, &img_comp, STBI_grey));
    if (!loaded || img_w <= 0 || img_h <= 0) {
        if (error_message != nullptr) {
            *error_message = "Failed to load font atlas PNG '" + image_path.string() + "'";
        }
        return false;
    }

    GameUiFontAtlas atlas{};
    atlas.width = static_cast<std::uint32_t>(img_w);
    atlas.height = static_cast<std::uint32_t>(img_h);
    atlas.base_pixel_size = root.value("base_pixel_size", 20.0F);
    atlas.cap_height_px = root.value("cap_height_px", 22.2F);
    atlas.line_height_px = root.value("line_height_px", 27.5F);
    atlas.sdf = root.value("sdf", true);
    atlas.sdf_spread_px = root.value("sdf_spread_px", 3.30F);
    if (root.contains("solid_uv_min") && root["solid_uv_min"].is_array() && root["solid_uv_min"].size() >= 2U) {
        atlas.solid_uv_min = glm::vec2(
            root["solid_uv_min"][0U].get<float>(), root["solid_uv_min"][1U].get<float>());
    }
    if (root.contains("solid_uv_max") && root["solid_uv_max"].is_array() && root["solid_uv_max"].size() >= 2U) {
        atlas.solid_uv_max = glm::vec2(
            root["solid_uv_max"][0U].get<float>(), root["solid_uv_max"][1U].get<float>());
    }

    const std::size_t pixel_count =
        static_cast<std::size_t>(img_w) * static_cast<std::size_t>(img_h);
    atlas.pixels_r8.assign(loaded.get(), loaded.get() + pixel_count);

    if (root.contains("glyphs") && root["glyphs"].is_array()) {
        atlas.glyphs.reserve(root["glyphs"].size());
        for (const auto& entry : root["glyphs"]) {
            if (!entry.is_object()) {
                continue;
            }
            GameUiGlyphMetrics g{};
            g.codepoint = entry.value("codepoint", 0U);
            if (entry.contains("uv_min") && entry["uv_min"].is_array() && entry["uv_min"].size() >= 2U) {
                g.uv_min = glm::vec2(entry["uv_min"][0U].get<float>(), entry["uv_min"][1U].get<float>());
            }
            if (entry.contains("uv_max") && entry["uv_max"].is_array() && entry["uv_max"].size() >= 2U) {
                g.uv_max = glm::vec2(entry["uv_max"][0U].get<float>(), entry["uv_max"][1U].get<float>());
            }
            if (entry.contains("size_px") && entry["size_px"].is_array() && entry["size_px"].size() >= 2U) {
                g.size_px = glm::vec2(entry["size_px"][0U].get<float>(), entry["size_px"][1U].get<float>());
            }
            if (entry.contains("bearing_px") && entry["bearing_px"].is_array() && entry["bearing_px"].size() >= 2U) {
                g.bearing_px = glm::vec2(
                    entry["bearing_px"][0U].get<float>(), entry["bearing_px"][1U].get<float>());
            }
            g.advance_px = entry.value("advance_px", 0.0F);
            atlas.glyphs.push_back(g);
        }
    }

    std::sort(
        atlas.glyphs.begin(),
        atlas.glyphs.end(),
        [](const GameUiGlyphMetrics& lhs, const GameUiGlyphMetrics& rhs) {
            return lhs.codepoint < rhs.codepoint;
        });
    atlas.rebuild_lookup();
    out_atlas = std::move(atlas);
    return true;
}

void UiScaler::configure(const glm::vec2 viewport_px, const UiScaleConfig& config) noexcept {
    viewport_px_.x = std::max(viewport_px.x, 1.0F);
    viewport_px_.y = std::max(viewport_px.y, 1.0F);
    const float ref_w = std::max(config.reference_resolution.x, 1.0F);
    const float ref_h = std::max(config.reference_resolution.y, 1.0F);
    const float ratio_x = viewport_px_.x / ref_w;
    const float ratio_y = viewport_px_.y / ref_h;

    float base_scale = 1.0F;
    switch (config.mode) {
    case UiScaleMode::fit:
        base_scale = std::min(ratio_x, ratio_y);
        break;
    case UiScaleMode::match_height:
        base_scale = ratio_y;
        break;
    case UiScaleMode::match_width:
        base_scale = ratio_x;
        break;
    case UiScaleMode::pixel_constant:
        base_scale = 1.0F;
        break;
    }

    const float user = std::clamp(config.user_scale, 0.4F, 3.0F);
    scale_ = std::clamp(base_scale * user, config.min_scale, config.max_scale);

    const float safe = std::clamp(config.safe_area_fraction, 0.0F, 0.25F);
    const float mx = viewport_px_.x * safe;
    const float my = viewport_px_.y * safe;
    safe_rect_ = UiRect{glm::vec2(mx, my), glm::vec2(viewport_px_.x - mx, viewport_px_.y - my)};
}

glm::vec2 UiScaler::anchor_point(const UiAnchorPreset preset, const bool respect_safe_area) const noexcept {
    const UiRect bounds = respect_safe_area
        ? safe_rect_
        : UiRect{glm::vec2(0.0F, 0.0F), viewport_px_};
    const float cx = (bounds.min.x + bounds.max.x) * 0.5F;
    const float cy = (bounds.min.y + bounds.max.y) * 0.5F;

    switch (preset) {
    case UiAnchorPreset::top_left:
        return bounds.min;
    case UiAnchorPreset::top_center:
        return glm::vec2(cx, bounds.min.y);
    case UiAnchorPreset::top_right:
        return glm::vec2(bounds.max.x, bounds.min.y);
    case UiAnchorPreset::middle_left:
        return glm::vec2(bounds.min.x, cy);
    case UiAnchorPreset::center:
        return glm::vec2(cx, cy);
    case UiAnchorPreset::middle_right:
        return glm::vec2(bounds.max.x, cy);
    case UiAnchorPreset::bottom_left:
        return glm::vec2(bounds.min.x, bounds.max.y);
    case UiAnchorPreset::bottom_center:
        return glm::vec2(cx, bounds.max.y);
    case UiAnchorPreset::bottom_right:
        return bounds.max;
    case UiAnchorPreset::stretch_fill:
        return bounds.min;
    }
    return bounds.min;
}

UiRect UiScaler::resolve(
    const UiAnchorPreset anchor,
    const glm::vec2 offset_ref,
    const glm::vec2 size_ref,
    const glm::vec2 pivot,
    const bool respect_safe_area) const noexcept {
    if (anchor == UiAnchorPreset::stretch_fill) {
        const UiRect bounds = respect_safe_area
            ? safe_rect_
            : UiRect{glm::vec2(0.0F, 0.0F), viewport_px_};
        const glm::vec2 inset = offset_ref * scale_;
        return UiRect{bounds.min + inset, glm::max(bounds.max - inset, bounds.min)};
    }
    const glm::vec2 anchor_px = anchor_point(anchor, respect_safe_area);
    const glm::vec2 size_px = size_ref * scale_;
    const glm::vec2 origin = anchor_px + offset_ref * scale_ - size_px * pivot;
    return UiRect{origin, origin + size_px};
}

void build_game_ui_font_atlas(
    std::array<std::uint8_t, game_ui_atlas_size * game_ui_atlas_size>& out_pixels) noexcept {
    out_pixels.fill(0U);

    // Slot 0: completely solid white block for rectangles/lines/arcs.
    for (std::uint32_t py = 0U; py < game_ui_glyph_cell_size; ++py) {
        for (std::uint32_t px = 0U; px < game_ui_glyph_cell_size; ++px) {
            out_pixels[py * game_ui_atlas_size + px] = 255U;
        }
    }

    // Slots 1..160: 5x7 glyphs upscaled 2x cleanly inside the 16x16 cell at (3, 1)..(13, 15).
    for (std::uint32_t slot = 1U; slot < total_glyph_slots; ++slot) {
        const GlyphBitmap glyph = bitmap_for_slot(slot);
        const std::uint32_t cell_x = (slot % game_ui_glyph_grid_dim) * game_ui_glyph_cell_size;
        const std::uint32_t cell_y = (slot / game_ui_glyph_grid_dim) * game_ui_glyph_cell_size;
        for (std::uint32_t row = 0U; row < 7U; ++row) {
            const std::uint8_t bits = glyph.rows[row];
            for (std::uint32_t col = 0U; col < 5U; ++col) {
                if (((bits >> (4U - col)) & 1U) == 0U) {
                    continue;
                }
                const std::uint32_t base_x = cell_x + 3U + col * 2U;
                const std::uint32_t base_y = cell_y + 1U + row * 2U;
                for (std::uint32_t dy = 0U; dy < 2U; ++dy) {
                    for (std::uint32_t dx = 0U; dx < 2U; ++dx) {
                        out_pixels[(base_y + dy) * game_ui_atlas_size + (base_x + dx)] = 255U;
                    }
                }
            }
        }
    }
}

glm::mat4 make_ui_view_projection(
    const glm::vec3& camera_pos,
    glm::vec3 camera_forward,
    glm::vec3 camera_up,
    const float aspect,
    const float near_plane,
    const float far_plane,
    const float vertical_fov_deg) noexcept {
    if (glm::dot(camera_forward, camera_forward) <= 1.0e-8F) {
        camera_forward = glm::vec3(0.0F, 0.0F, -1.0F);
    } else {
        camera_forward = glm::normalize(camera_forward);
    }
    if (glm::dot(camera_up, camera_up) <= 1.0e-8F ||
        std::abs(glm::dot(camera_forward, glm::normalize(camera_up))) > 0.999F) {
        camera_up = std::abs(camera_forward.y) < 0.9F ? glm::vec3(0.0F, 1.0F, 0.0F)
                                                     : glm::vec3(0.0F, 0.0F, 1.0F);
    } else {
        camera_up = glm::normalize(camera_up);
    }
    const glm::mat4 view = glm::lookAtRH(camera_pos, camera_pos + camera_forward, camera_up);
    glm::mat4 proj = (std::abs(vertical_fov_deg - camera_vertical_fov) < 0.05F)
        ? camera_projection(std::max(aspect, 0.01F), near_plane, far_plane)
        : glm::perspectiveRH_ZO(
              glm::radians(std::clamp(vertical_fov_deg, 15.0F, 120.0F)),
              std::max(aspect, 0.01F),
              far_plane,
              near_plane);
    proj[1][1] *= -1.0F;
    return proj * view;
}

std::optional<glm::vec2> project_world_to_screen(
    const glm::vec3& world_pos,
    const glm::mat4& view_projection,
    const glm::vec2 viewport_px) noexcept {
    if (viewport_px.x <= 0.0F || viewport_px.y <= 0.0F) {
        return std::nullopt;
    }
    const glm::vec4 clip = view_projection * glm::vec4(world_pos, 1.0F);
    if (!std::isfinite(clip.w) || clip.w <= 1.0e-4F) {
        return std::nullopt;
    }
    const float inv_w = 1.0F / clip.w;
    const glm::vec3 ndc(clip.x * inv_w, clip.y * inv_w, clip.z * inv_w);
    if (!std::isfinite(ndc.x) || !std::isfinite(ndc.y) || !std::isfinite(ndc.z) ||
        ndc.z < 0.0F || ndc.z > 1.0F) {
        return std::nullopt;
    }
    return glm::vec2(
        (ndc.x * 0.5F + 0.5F) * viewport_px.x,
        (ndc.y * 0.5F + 0.5F) * viewport_px.y);
}

WorldScreenProjection project_world_indicator(
    const glm::vec3& world_pos,
    const glm::vec3& camera_pos,
    const glm::mat4& view_projection,
    const glm::vec2 viewport_px,
    const float edge_margin_px,
    const bool clamp_to_screen) noexcept {
    WorldScreenProjection out{};
    out.distance_m = glm::length(world_pos - camera_pos);
    if (viewport_px.x <= 2.0F || viewport_px.y <= 2.0F) {
        return out;
    }

    const glm::vec2 center = viewport_px * 0.5F;
    const glm::vec4 clip = view_projection * glm::vec4(world_pos, 1.0F);
    out.in_front = std::isfinite(clip.w) && clip.w > 1.0e-4F;

    glm::vec2 raw_screen = center;
    bool depth_valid = false;
    if (out.in_front) {
        const float inv_w = 1.0F / clip.w;
        const float ndc_x = clip.x * inv_w;
        const float ndc_y = clip.y * inv_w;
        const float ndc_z = clip.z * inv_w;
        depth_valid = std::isfinite(ndc_x) && std::isfinite(ndc_y) && std::isfinite(ndc_z) &&
            ndc_z >= 0.0F && ndc_z <= 1.0F;
        raw_screen = glm::vec2(
            (ndc_x * 0.5F + 0.5F) * viewport_px.x,
            (ndc_y * 0.5F + 0.5F) * viewport_px.y);
    } else {
        // Flip XY when behind near plane so clamped directional arrow points toward target
        const float inv_abs_w = 1.0F / std::max(std::abs(clip.w), 1.0e-4F);
        const float mirror_x = -clip.x * inv_abs_w;
        const float mirror_y = -clip.y * inv_abs_w;
        raw_screen = glm::vec2(
            (mirror_x * 0.5F + 0.5F) * viewport_px.x,
            (mirror_y * 0.5F + 0.5F) * viewport_px.y);
    }

    const float margin = std::clamp(edge_margin_px, 4.0F, std::min(viewport_px.x, viewport_px.y) * 0.45F);
    const bool inside_bounds = raw_screen.x >= margin && raw_screen.x <= viewport_px.x - margin &&
        raw_screen.y >= margin && raw_screen.y <= viewport_px.y - margin;

    if (out.in_front && depth_valid && inside_bounds) {
        out.screen_px = raw_screen;
        out.visible_on_screen = true;
        out.clamped_to_edge = false;
        const glm::vec2 d = raw_screen - center;
        const float len = glm::length(d);
        out.edge_dir = len > 1.0e-4F ? d / len : glm::vec2(0.0F, -1.0F);
        return out;
    }

    if (!clamp_to_screen) {
        out.screen_px = raw_screen;
        out.visible_on_screen = out.in_front && depth_valid &&
            raw_screen.x >= 0.0F && raw_screen.x <= viewport_px.x &&
            raw_screen.y >= 0.0F && raw_screen.y <= viewport_px.y;
        return out;
    }

    glm::vec2 dir = raw_screen - center;
    const float len = glm::length(dir);
    dir = len > 1.0e-4F ? (dir / len) : glm::vec2(0.0F, -1.0F);
    out.edge_dir = dir;

    const float half_w = std::max(center.x - margin, 1.0F);
    const float half_h = std::max(center.y - margin, 1.0F);
    const float tx = std::abs(dir.x) > 1.0e-5F ? (half_w / std::abs(dir.x)) : 1.0e9F;
    const float ty = std::abs(dir.y) > 1.0e-5F ? (half_h / std::abs(dir.y)) : 1.0e9F;
    const float t = std::min(tx, ty);
    out.screen_px = center + dir * t;
    out.clamped_to_edge = true;
    out.visible_on_screen = false;
    return out;
}

GameOverlayCanvas::GameOverlayCanvas()
    : atlas_(&default_game_ui_font_atlas()) {
    vertices_.reserve(max_vertices_);
}

void GameOverlayCanvas::reserve(const std::size_t max_vertices) {
    max_vertices_ = std::max<std::size_t>(max_vertices, 64U);
    vertices_.reserve(max_vertices_);
}

void GameOverlayCanvas::clear() noexcept {
    vertices_.clear();
    truncated_ = false;
}

void GameOverlayCanvas::set_viewport(const float width_px, const float height_px) noexcept {
    viewport_width_ = std::max(width_px, 1.0F);
    viewport_height_ = std::max(height_px, 1.0F);
}

void GameOverlayCanvas::set_font_atlas(const GameUiFontAtlas* atlas) noexcept {
    atlas_ = atlas != nullptr ? atlas : &default_game_ui_font_atlas();
}

void GameOverlayCanvas::push_quad(
    const glm::vec2 p0,
    const glm::vec2 p1,
    const glm::vec2 p2,
    const glm::vec2 p3,
    const glm::vec2 uv_min,
    const glm::vec2 uv_max,
    const std::uint32_t rgba8) noexcept {
    if (max_vertices_ < 6U || vertices_.size() > max_vertices_ - 6U) {
        truncated_ = true;
        return;
    }
    vertices_.push_back(GameUiVertex{p0, glm::vec2(uv_min.x, uv_min.y), rgba8});
    vertices_.push_back(GameUiVertex{p1, glm::vec2(uv_max.x, uv_min.y), rgba8});
    vertices_.push_back(GameUiVertex{p2, glm::vec2(uv_max.x, uv_max.y), rgba8});
    vertices_.push_back(GameUiVertex{p0, glm::vec2(uv_min.x, uv_min.y), rgba8});
    vertices_.push_back(GameUiVertex{p2, glm::vec2(uv_max.x, uv_max.y), rgba8});
    vertices_.push_back(GameUiVertex{p3, glm::vec2(uv_min.x, uv_max.y), rgba8});
}

void GameOverlayCanvas::push_solid_quad(
    const glm::vec2 p0,
    const glm::vec2 p1,
    const glm::vec2 p2,
    const glm::vec2 p3,
    const std::uint32_t rgba8) noexcept {
    const glm::vec2 uv = atlas_ != nullptr ? (atlas_->solid_uv_min + atlas_->solid_uv_max) * 0.5F : solid_white_uv();
    push_quad(p0, p1, p2, p3, uv, uv, rgba8);
}

void GameOverlayCanvas::draw_rounded_panel(
    const glm::vec2 min_px,
    const glm::vec2 max_px,
    const std::uint32_t fill_rgba8,
    const std::uint32_t border_rgba8,
    const float border_thickness_px) noexcept {
    if (max_px.x <= min_px.x || max_px.y <= min_px.y) {
        return;
    }
    if ((fill_rgba8 >> 24U) != 0U) {
        draw_rect(min_px, max_px, fill_rgba8);
    }
    if ((border_rgba8 >> 24U) != 0U && border_thickness_px > 0.0F) {
        draw_rect_outline(min_px, max_px, border_thickness_px, border_rgba8);
    }
}

void GameOverlayCanvas::draw_crosshair(
    const glm::vec2 center_px,
    const UiCrosshairStyle& style) noexcept {
    const float gap = std::max(style.gap_px, 0.0F);
    const float arm = std::max(style.arm_length_px, 0.0F);
    const float t = std::max(style.thickness_px, 1.0F);
    const float half_t = t * 0.5F;
    const bool has_outline = (style.outline_rgba8 >> 24U) != 0U;

    const auto draw_arm = [&](const glm::vec2 a, const glm::vec2 b) {
        if (has_outline) {
            draw_rect(a - glm::vec2(1.0F), b + glm::vec2(1.0F), style.outline_rgba8);
        }
        draw_rect(a, b, style.color_rgba8);
    };

    if (arm > 0.0F) {
        draw_arm(glm::vec2(center_px.x - gap - arm, center_px.y - half_t),
                 glm::vec2(center_px.x - gap, center_px.y + half_t));
        draw_arm(glm::vec2(center_px.x + gap, center_px.y - half_t),
                 glm::vec2(center_px.x + gap + arm, center_px.y + half_t));
        draw_arm(glm::vec2(center_px.x - half_t, center_px.y - gap - arm),
                 glm::vec2(center_px.x + half_t, center_px.y - gap));
        draw_arm(glm::vec2(center_px.x - half_t, center_px.y + gap),
                 glm::vec2(center_px.x + half_t, center_px.y + gap + arm));
    }
    if (style.center_dot_radius_px > 0.0F) {
        const float r = style.center_dot_radius_px;
        draw_arm(center_px - glm::vec2(r), center_px + glm::vec2(r));
    }
}

void GameOverlayCanvas::draw_rect(
    const glm::vec2 min_px,
    const glm::vec2 max_px,
    const std::uint32_t rgba8) noexcept {
    if ((rgba8 >> 24U) == 0U || max_px.x <= min_px.x || max_px.y <= min_px.y) {
        return;
    }
    push_solid_quad(
        min_px,
        glm::vec2(max_px.x, min_px.y),
        max_px,
        glm::vec2(min_px.x, max_px.y),
        rgba8);
}

void GameOverlayCanvas::draw_rect_outline(
    const glm::vec2 min_px,
    const glm::vec2 max_px,
    const float thickness_px,
    const std::uint32_t rgba8) noexcept {
    if ((rgba8 >> 24U) == 0U || thickness_px <= 0.0F ||
        max_px.x <= min_px.x || max_px.y <= min_px.y) {
        return;
    }
    const float t = std::min(thickness_px, 0.5F * std::min(max_px.x - min_px.x, max_px.y - min_px.y));
    draw_rect(min_px, glm::vec2(max_px.x, min_px.y + t), rgba8);
    draw_rect(glm::vec2(min_px.x, max_px.y - t), max_px, rgba8);
    draw_rect(glm::vec2(min_px.x, min_px.y + t), glm::vec2(min_px.x + t, max_px.y - t), rgba8);
    draw_rect(glm::vec2(max_px.x - t, min_px.y + t), glm::vec2(max_px.x, max_px.y - t), rgba8);
}

void GameOverlayCanvas::draw_line(
    const glm::vec2 start_px,
    const glm::vec2 end_px,
    const float thickness_px,
    const std::uint32_t rgba8) noexcept {
    if ((rgba8 >> 24U) == 0U || thickness_px <= 0.0F) {
        return;
    }
    const glm::vec2 delta = end_px - start_px;
    const float len_sq = delta.x * delta.x + delta.y * delta.y;
    if (len_sq <= 1.0e-6F) {
        return;
    }
    const float inv_len = 1.0F / std::sqrt(len_sq);
    const glm::vec2 normal(-delta.y * inv_len * (thickness_px * 0.5F),
                           delta.x * inv_len * (thickness_px * 0.5F));
    push_solid_quad(
        start_px - normal,
        end_px - normal,
        end_px + normal,
        start_px + normal,
        rgba8);
}

void GameOverlayCanvas::draw_progress_bar(
    const glm::vec2 min_px,
    const glm::vec2 max_px,
    const float fraction,
    const std::uint32_t bg_rgba8,
    const std::uint32_t fill_rgba8,
    const std::uint32_t border_rgba8,
    const float border_thickness_px) noexcept {
    draw_rect(min_px, max_px, bg_rgba8);
    const float clamped = std::clamp(std::isfinite(fraction) ? fraction : 0.0F, 0.0F, 1.0F);
    if (clamped > 0.0F) {
        const float fill_x = min_px.x + (max_px.x - min_px.x) * clamped;
        draw_rect(min_px, glm::vec2(fill_x, max_px.y), fill_rgba8);
    }
    if ((border_rgba8 >> 24U) != 0U && border_thickness_px > 0.0F) {
        draw_rect_outline(min_px, max_px, border_thickness_px, border_rgba8);
    }
}

void GameOverlayCanvas::draw_arc(
    const glm::vec2 center_px,
    const float radius_px,
    const float thickness_px,
    const float start_radians,
    const float end_radians,
    const std::uint32_t rgba8,
    const std::uint32_t segments) noexcept {
    if ((rgba8 >> 24U) == 0U || radius_px <= 0.0F || thickness_px <= 0.0F || segments == 0U) {
        return;
    }
    const std::uint32_t count = std::min<std::uint32_t>(segments, 128U);
    const float half_t = thickness_px * 0.5F;
    const float r_inner = std::max(0.0F, radius_px - half_t);
    const float r_outer = radius_px + half_t;
    const float step = (end_radians - start_radians) / static_cast<float>(count);
    for (std::uint32_t i = 0U; i < count; ++i) {
        const float a0 = start_radians + static_cast<float>(i) * step;
        const float a1 = a0 + step;
        const glm::vec2 d0(std::cos(a0), std::sin(a0));
        const glm::vec2 d1(std::cos(a1), std::sin(a1));
        push_solid_quad(
            center_px + d0 * r_inner,
            center_px + d1 * r_inner,
            center_px + d1 * r_outer,
            center_px + d0 * r_outer,
            rgba8);
    }
}

void GameOverlayCanvas::draw_text(
    const glm::vec2 top_left_px,
    const std::string_view utf8_text,
    const float scale,
    const std::uint32_t rgba8) noexcept {
    if (scale <= 0.0F) {
        return;
    }
    draw_text_px(top_left_px, utf8_text, 14.0F * scale, rgba8, false);
}

void GameOverlayCanvas::draw_text_shadowed(
    const glm::vec2 top_left_px,
    const std::string_view utf8_text,
    const float scale,
    const std::uint32_t rgba8,
    const std::uint32_t shadow_rgba8,
    const glm::vec2 shadow_offset_px) noexcept {
    if ((rgba8 >> 24U) == 0U || scale <= 0.0F || utf8_text.empty()) {
        return;
    }
    if ((shadow_rgba8 >> 24U) != 0U) {
        draw_text(top_left_px + shadow_offset_px, utf8_text, scale, shadow_rgba8);
    }
    draw_text(top_left_px, utf8_text, scale, rgba8);
}

void GameOverlayCanvas::draw_text_px(
    const glm::vec2 top_left_px,
    const std::string_view utf8_text,
    const float height_px,
    const std::uint32_t rgba8,
    const bool shadowed) noexcept {
    if ((rgba8 >> 24U) == 0U || height_px <= 0.0F || utf8_text.empty()) {
        return;
    }
    if (shadowed) {
        const std::uint8_t src_a = static_cast<std::uint8_t>(rgba8 >> 24U);
        const std::uint32_t shadow = pack_rgba8(0U, 0U, 0U, static_cast<std::uint8_t>((static_cast<std::uint32_t>(src_a) * 195U) / 255U));
        const float offset = std::clamp(height_px * 0.075F, 1.0F, 3.0F);
        draw_text_px(top_left_px + glm::vec2(offset, offset), utf8_text, height_px, shadow, false);
    }

    const GameUiFontAtlas* atlas = atlas_;
    if (atlas != nullptr && !atlas->glyphs.empty()) {
        const float unit_scale = height_px / std::max(atlas->cap_height_px, 1.0F);
        const float line_h = atlas->line_height_px * unit_scale;
        const GameUiGlyphMetrics* space_glyph = atlas->find_glyph(' ');
        const float space_adv = (space_glyph != nullptr ? space_glyph->advance_px : 9.0F) * unit_scale;

        float cursor_x = top_left_px.x;
        float cursor_y = top_left_px.y;
        std::size_t offset = 0U;
        while (offset < utf8_text.size()) {
            const std::uint32_t cp = decode_utf8(utf8_text, offset);
            if (cp == '\n') {
                cursor_x = top_left_px.x;
                cursor_y += line_h;
                continue;
            }
            if (cp == ' ') {
                cursor_x += space_adv;
                continue;
            }
            if (cp == '\t') {
                cursor_x += space_adv * 4.0F;
                continue;
            }
            if (cp < 0x20U) {
                continue;
            }
            const GameUiGlyphMetrics* glyph = atlas->find_glyph(cp);
            if (glyph == nullptr) {
                glyph = &atlas->fallback_glyph();
            }
            if (glyph->size_px.x > 0.0F && glyph->size_px.y > 0.0F) {
                const glm::vec2 g_pos = glm::vec2(cursor_x, cursor_y) + glyph->bearing_px * unit_scale;
                const glm::vec2 g_size = glyph->size_px * unit_scale;
                push_quad(
                    g_pos,
                    glm::vec2(g_pos.x + g_size.x, g_pos.y),
                    g_pos + g_size,
                    glm::vec2(g_pos.x, g_pos.y + g_size.y),
                    glyph->uv_min,
                    glyph->uv_max,
                    rgba8);
            }
            cursor_x += glyph->advance_px * unit_scale;
        }
        return;
    }

    // Legacy 5x7 fallback path if caller explicitly cleared atlas
    const float scale = height_px / 14.0F;
    const float glyph_w = 10.0F * scale;
    const float glyph_h = 14.0F * scale;
    const float advance_x = 12.0F * scale;
    const float line_h = 16.0F * scale;

    float cursor_x = top_left_px.x;
    float cursor_y = top_left_px.y;
    std::size_t offset = 0U;
    while (offset < utf8_text.size()) {
        const std::uint32_t cp = decode_utf8(utf8_text, offset);
        if (cp == '\n') {
            cursor_x = top_left_px.x;
            cursor_y += line_h;
            continue;
        }
        if (cp == ' ') {
            cursor_x += advance_x;
            continue;
        }
        if (cp == '\t') {
            cursor_x += advance_x * 4.0F;
            continue;
        }
        if (cp < 0x20U) {
            continue;
        }
        const std::uint32_t slot = slot_for_codepoint(cp);
        glm::vec2 uv_min{};
        glm::vec2 uv_max{};
        slot_uv_bounds(slot, uv_min, uv_max);
        const glm::vec2 p0(cursor_x, cursor_y);
        const glm::vec2 p1(cursor_x + glyph_w, cursor_y);
        const glm::vec2 p2(cursor_x + glyph_w, cursor_y + glyph_h);
        const glm::vec2 p3(cursor_x, cursor_y + glyph_h);
        push_quad(p0, p1, p2, p3, uv_min, uv_max, rgba8);
        cursor_x += advance_x;
    }
}

namespace {

template <typename LineFn>
void for_each_wrapped_line(
    const GameOverlayCanvas& canvas,
    const std::string_view utf8_text,
    const float max_width_px,
    const float height_px,
    LineFn&& on_line) noexcept {
    if (utf8_text.empty() || height_px <= 0.0F) {
        return;
    }
    const float limit_w = max_width_px > 1.0F ? max_width_px : 1.0e9F;
    std::size_t pos = 0U;
    while (pos < utf8_text.size()) {
        const std::size_t nl = utf8_text.find('\n', pos);
        const std::size_t para_end = nl == std::string_view::npos ? utf8_text.size() : nl;
        std::size_t line_start = pos;

        while (line_start < para_end) {
            std::size_t cursor = line_start;
            std::size_t last_space = std::string_view::npos;
            std::size_t last_fit = line_start;
            bool wrapped = false;

            while (cursor < para_end) {
                const std::size_t prev = cursor;
                const std::uint32_t cp = decode_utf8(utf8_text, cursor);
                if (cp == ' ' || cp == '\t') {
                    last_space = prev;
                }
                const float w = canvas.measure_text_px(
                    utf8_text.substr(line_start, cursor - line_start), height_px).x;
                if (w > limit_w && last_fit > line_start) {
                    if (last_space != std::string_view::npos && last_space > line_start) {
                        on_line(utf8_text.substr(line_start, last_space - line_start));
                        line_start = last_space + 1U;
                        while (line_start < para_end && utf8_text[line_start] == ' ') {
                            ++line_start;
                        }
                    } else {
                        on_line(utf8_text.substr(line_start, last_fit - line_start));
                        line_start = last_fit;
                    }
                    wrapped = true;
                    break;
                }
                last_fit = cursor;
            }
            if (!wrapped) {
                on_line(utf8_text.substr(line_start, para_end - line_start));
                line_start = para_end;
            }
        }

        if (nl == std::string_view::npos) {
            break;
        }
        if (para_end == pos) {
            on_line(std::string_view{});
        }
        pos = nl + 1U;
    }
}

} // namespace

void GameOverlayCanvas::draw_text_wrapped(
    const glm::vec2 top_left_px,
    const float max_width_px,
    const std::string_view utf8_text,
    const float height_px,
    const std::uint32_t rgba8,
    const float line_spacing,
    const bool shadowed) noexcept {
    if ((rgba8 >> 24U) == 0U || height_px <= 0.0F || utf8_text.empty()) {
        return;
    }
    const float step_y = height_px * std::clamp(line_spacing, 0.9F, 2.5F);
    float y = top_left_px.y;
    for_each_wrapped_line(*this, utf8_text, max_width_px, height_px, [&](const std::string_view line) {
        if (!line.empty()) {
            draw_text_px(glm::vec2(top_left_px.x, y), line, height_px, rgba8, shadowed);
        }
        y += step_y;
    });
}

glm::vec2 GameOverlayCanvas::measure_text(
    const std::string_view utf8_text,
    const float scale) const noexcept {
    if (scale <= 0.0F) {
        return glm::vec2(0.0F);
    }
    return measure_text_px(utf8_text, 14.0F * scale);
}

glm::vec2 GameOverlayCanvas::measure_text_px(
    const std::string_view utf8_text,
    const float height_px) const noexcept {
    if (height_px <= 0.0F || utf8_text.empty()) {
        return glm::vec2(0.0F);
    }
    const GameUiFontAtlas* atlas = atlas_;
    if (atlas != nullptr && !atlas->glyphs.empty()) {
        const float unit_scale = height_px / std::max(atlas->cap_height_px, 1.0F);
        const float line_h = atlas->line_height_px * unit_scale;
        const GameUiGlyphMetrics* space_glyph = atlas->find_glyph(' ');
        const float space_adv = (space_glyph != nullptr ? space_glyph->advance_px : 9.0F) * unit_scale;

        float max_w = 0.0F;
        float line_w = 0.0F;
        std::uint32_t lines = 1U;
        std::size_t offset = 0U;
        while (offset < utf8_text.size()) {
            const std::uint32_t cp = decode_utf8(utf8_text, offset);
            if (cp == '\n') {
                max_w = std::max(max_w, line_w);
                line_w = 0.0F;
                ++lines;
            } else if (cp == ' ') {
                line_w += space_adv;
            } else if (cp == '\t') {
                line_w += space_adv * 4.0F;
            } else if (cp >= 0x20U) {
                const GameUiGlyphMetrics* glyph = atlas->find_glyph(cp);
                if (glyph == nullptr) {
                    glyph = &atlas->fallback_glyph();
                }
                line_w += glyph->advance_px * unit_scale;
            }
        }
        max_w = std::max(max_w, line_w);
        return glm::vec2(max_w, static_cast<float>(lines) * line_h);
    }

    const float scale = height_px / 14.0F;
    const float advance_x = 12.0F * scale;
    const float line_h = 16.0F * scale;
    float max_w = 0.0F;
    float line_w = 0.0F;
    std::uint32_t lines = 1U;
    std::size_t offset = 0U;
    while (offset < utf8_text.size()) {
        const std::uint32_t cp = decode_utf8(utf8_text, offset);
        if (cp == '\n') {
            max_w = std::max(max_w, line_w);
            line_w = 0.0F;
            ++lines;
        } else if (cp == '\t') {
            line_w += advance_x * 4.0F;
        } else if (cp >= 0x20U) {
            line_w += advance_x;
        }
    }
    max_w = std::max(max_w, line_w);
    return glm::vec2(max_w, static_cast<float>(lines) * line_h);
}

glm::vec2 GameOverlayCanvas::measure_text_wrapped(
    const std::string_view utf8_text,
    const float max_width_px,
    const float height_px,
    const float line_spacing) const noexcept {
    if (height_px <= 0.0F || utf8_text.empty()) {
        return glm::vec2(0.0F);
    }
    const float step_y = height_px * std::clamp(line_spacing, 0.9F, 2.5F);
    float max_w = 0.0F;
    std::uint32_t line_count = 0U;
    for_each_wrapped_line(*this, utf8_text, max_width_px, height_px, [&](const std::string_view line) {
        max_w = std::max(max_w, measure_text_px(line, height_px).x);
        ++line_count;
    });
    return glm::vec2(max_w, static_cast<float>(line_count) * step_y);
}

} // namespace vcr
