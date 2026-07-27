#include "editor_ui_kit.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace vcr {
namespace {

[[nodiscard]] ImVec4 rgba(
    const int red,
    const int green,
    const int blue,
    const int alpha = 255) noexcept {
    constexpr float inverse_byte = 1.0F / 255.0F;
    const auto srgb_to_linear = [](const int component) noexcept {
        constexpr float inverse_component =
            1.0F / 255.0F;
        const float srgb =
            static_cast<float>(component) *
            inverse_component;
        return srgb <= 0.04045F
            ? srgb / 12.92F
            : std::pow(
                (srgb + 0.055F) / 1.055F,
                2.4F);
    };
    return ImVec4(
        srgb_to_linear(red),
        srgb_to_linear(green),
        srgb_to_linear(blue),
        static_cast<float>(alpha) * inverse_byte);
}

[[nodiscard]] ImVec4 with_alpha(
    ImVec4 color,
    const float alpha) noexcept {
    color.w *= alpha;
    return color;
}

[[nodiscard]] ImVec4 mix(
    const ImVec4 lhs,
    const ImVec4 rhs,
    const float amount) noexcept {
    const float clamped = std::clamp(amount, 0.0F, 1.0F);
    return ImVec4(
        lhs.x + (rhs.x - lhs.x) * clamped,
        lhs.y + (rhs.y - lhs.y) * clamped,
        lhs.z + (rhs.z - lhs.z) * clamped,
        lhs.w + (rhs.w - lhs.w) * clamped);
}

[[nodiscard]] ImU32 packed(
    ImVec4 color,
    const float alpha = 1.0F) noexcept {
    color.w *= alpha * ImGui::GetStyle().Alpha;
    return ImGui::ColorConvertFloat4ToU32(color);
}

[[nodiscard]] ImVec4 surface_canvas() noexcept {
    return rgba(24, 26, 31);
}

[[nodiscard]] ImVec4 surface_deep() noexcept {
    return rgba(33, 37, 43);
}

[[nodiscard]] ImVec4 surface_panel() noexcept {
    return rgba(40, 44, 52);
}

[[nodiscard]] ImVec4 surface_raised() noexcept {
    return rgba(44, 49, 58);
}

[[nodiscard]] ImVec4 surface_hovered() noexcept {
    return rgba(50, 56, 66);
}

[[nodiscard]] ImVec4 surface_active() noexcept {
    return rgba(62, 68, 81);
}

[[nodiscard]] ImVec4 one_dark_text() noexcept {
    return rgba(171, 178, 191);
}

[[nodiscard]] ImVec4 one_dark_bright() noexcept {
    return rgba(215, 218, 224);
}

[[nodiscard]] ImVec4 one_dark_muted() noexcept {
    return rgba(127, 132, 142);
}

[[nodiscard]] ImVec4 one_dark_comment() noexcept {
    return rgba(92, 99, 112);
}

[[nodiscard]] ImVec4 one_dark_border() noexcept {
    return rgba(62, 68, 81);
}

[[nodiscard]] ImVec4 one_dark_blue() noexcept {
    return rgba(97, 175, 239);
}

[[nodiscard]] ImVec4 one_dark_cyan() noexcept {
    return rgba(86, 182, 194);
}

[[nodiscard]] ImVec4 one_dark_green() noexcept {
    return rgba(152, 195, 121);
}

[[nodiscard]] ImVec4 one_dark_yellow() noexcept {
    return rgba(229, 192, 123);
}

[[nodiscard]] ImVec4 one_dark_red() noexcept {
    return rgba(224, 108, 117);
}

[[nodiscard]] ImVec4 one_dark_purple() noexcept {
    return rgba(198, 120, 221);
}

[[nodiscard]] bool point_inside(
    const ImVec2 point,
    const ImVec2 minimum,
    const ImVec2 maximum) noexcept {
    return point.x >= minimum.x && point.y >= minimum.y &&
        point.x < maximum.x && point.y < maximum.y;
}

[[nodiscard]] ImVec2 add(
    const ImVec2 lhs,
    const float x,
    const float y) noexcept {
    return ImVec2(lhs.x + x, lhs.y + y);
}

[[nodiscard]] std::optional<std::filesystem::path>
find_editor_font() {
#if defined(_WIN32)
    std::array<char, MAX_PATH + 1U> windows_directory{};
    const UINT length = GetWindowsDirectoryA(
        windows_directory.data(),
        static_cast<UINT>(windows_directory.size()));
    if (length > 0U &&
        static_cast<std::size_t>(length) <
            windows_directory.size()) {
        const std::filesystem::path candidate =
            std::filesystem::path(windows_directory.data()) /
            "Fonts" /
            "segoeui.ttf";
        std::error_code filesystem_error;
        if (std::filesystem::is_regular_file(
                candidate, filesystem_error) &&
            !filesystem_error) {
            return candidate;
        }
    }
#else
    constexpr std::array<const char*, 3U> candidates{
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation2/"
        "LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/ubuntu/Ubuntu-R.ttf",
    };
    for (const char* const path : candidates) {
        std::error_code filesystem_error;
        const std::filesystem::path candidate(path);
        if (std::filesystem::is_regular_file(
                candidate, filesystem_error) &&
            !filesystem_error) {
            return candidate;
        }
    }
#endif
    return std::nullopt;
}

} // namespace

void EditorUiKit::initialize() {
    ImGuiIO& io = ImGui::GetIO();
    ImFontConfig font_config{};
    font_config.SizePixels = 16.0F;
    font_config.PixelSnapH = false;
    font_config.RasterizerMultiply = 1.06F;
    if (const auto font_path = find_editor_font()) {
        const std::string encoded_path =
            font_path->string();
        body_font_ = io.Fonts->AddFontFromFileTTF(
            encoded_path.c_str(),
            font_config.SizePixels,
            &font_config,
            io.Fonts->GetGlyphRangesCyrillic());
    }
    if (body_font_ == nullptr) {
        body_font_ =
            io.Fonts->AddFontDefaultVector(&font_config);
    }
    if (body_font_ != nullptr) {
        io.FontDefault = body_font_;
    }

    ImGuiStyle& style = ImGui::GetStyle();
    style.FontSizeBase = 16.0F;
    style.Alpha = 1.0F;
    style.DisabledAlpha = 0.52F;
    style.WindowPadding = ImVec2(11.0F, 10.0F);
    style.WindowRounding = 6.0F;
    style.WindowBorderSize = 1.0F;
    style.WindowMinSize = ImVec2(120.0F, 64.0F);
    style.ChildRounding = 5.0F;
    style.ChildBorderSize = 1.0F;
    style.PopupRounding = 6.0F;
    style.PopupBorderSize = 1.0F;
    style.FramePadding = ImVec2(8.0F, 5.0F);
    style.FrameRounding = 4.0F;
    style.FrameBorderSize = 0.0F;
    style.ItemSpacing = ImVec2(8.0F, 7.0F);
    style.ItemInnerSpacing = ImVec2(6.0F, 5.0F);
    style.CellPadding = ImVec2(7.0F, 5.0F);
    style.IndentSpacing = 18.0F;
    style.ColumnsMinSpacing = 8.0F;
    style.ScrollbarSize = 11.0F;
    style.ScrollbarRounding = 8.0F;
    style.GrabMinSize = 10.0F;
    style.GrabRounding = 4.0F;
    style.TabRounding = 4.0F;
    style.TabBorderSize = 0.0F;
    style.TabBarBorderSize = 1.0F;
    style.TabBarOverlineSize = 2.0F;
    style.SeparatorTextBorderSize = 1.0F;
    style.SeparatorTextAlign = ImVec2(0.0F, 0.5F);
    style.SeparatorTextPadding = ImVec2(8.0F, 4.0F);
    style.DockingSeparatorSize = 1.0F;

    ImVec4* const colors = style.Colors;
    colors[ImGuiCol_Text] = one_dark_text();
    colors[ImGuiCol_TextDisabled] = one_dark_comment();
    colors[ImGuiCol_WindowBg] = surface_deep();
    colors[ImGuiCol_ChildBg] = surface_deep();
    colors[ImGuiCol_PopupBg] = with_alpha(surface_panel(), 0.98F);
    colors[ImGuiCol_Border] = one_dark_border();
    colors[ImGuiCol_BorderShadow] = rgba(0, 0, 0, 0);
    colors[ImGuiCol_FrameBg] = surface_panel();
    colors[ImGuiCol_FrameBgHovered] = surface_hovered();
    colors[ImGuiCol_FrameBgActive] = surface_active();
    colors[ImGuiCol_TitleBg] = surface_canvas();
    colors[ImGuiCol_TitleBgActive] = surface_deep();
    colors[ImGuiCol_TitleBgCollapsed] = surface_canvas();
    colors[ImGuiCol_MenuBarBg] = surface_deep();
    colors[ImGuiCol_ScrollbarBg] = surface_deep();
    colors[ImGuiCol_ScrollbarGrab] = surface_active();
    colors[ImGuiCol_ScrollbarGrabHovered] =
        mix(surface_active(), one_dark_blue(), 0.22F);
    colors[ImGuiCol_ScrollbarGrabActive] =
        mix(surface_active(), one_dark_blue(), 0.38F);
    colors[ImGuiCol_CheckMark] = one_dark_blue();
    colors[ImGuiCol_SliderGrab] = one_dark_cyan();
    colors[ImGuiCol_SliderGrabActive] = one_dark_blue();
    colors[ImGuiCol_Button] = surface_raised();
    colors[ImGuiCol_ButtonHovered] = surface_hovered();
    colors[ImGuiCol_ButtonActive] = surface_active();
    colors[ImGuiCol_Header] =
        mix(surface_panel(), one_dark_blue(), 0.12F);
    colors[ImGuiCol_HeaderHovered] =
        mix(surface_hovered(), one_dark_blue(), 0.18F);
    colors[ImGuiCol_HeaderActive] =
        mix(surface_active(), one_dark_blue(), 0.24F);
    colors[ImGuiCol_Separator] = one_dark_border();
    colors[ImGuiCol_SeparatorHovered] = one_dark_cyan();
    colors[ImGuiCol_SeparatorActive] = one_dark_blue();
    colors[ImGuiCol_ResizeGrip] =
        with_alpha(one_dark_blue(), 0.18F);
    colors[ImGuiCol_ResizeGripHovered] =
        with_alpha(one_dark_blue(), 0.55F);
    colors[ImGuiCol_ResizeGripActive] = one_dark_blue();
    colors[ImGuiCol_InputTextCursor] = one_dark_blue();
    colors[ImGuiCol_TabHovered] = surface_hovered();
    colors[ImGuiCol_Tab] = surface_deep();
    colors[ImGuiCol_TabSelected] = surface_panel();
    colors[ImGuiCol_TabSelectedOverline] = one_dark_blue();
    colors[ImGuiCol_TabDimmed] = surface_canvas();
    colors[ImGuiCol_TabDimmedSelected] = surface_deep();
    colors[ImGuiCol_TabDimmedSelectedOverline] =
        one_dark_comment();
    colors[ImGuiCol_DockingPreview] =
        with_alpha(one_dark_blue(), 0.55F);
    colors[ImGuiCol_DockingEmptyBg] = surface_canvas();
    colors[ImGuiCol_PlotLines] = one_dark_cyan();
    colors[ImGuiCol_PlotLinesHovered] = one_dark_blue();
    colors[ImGuiCol_PlotHistogram] = one_dark_green();
    colors[ImGuiCol_PlotHistogramHovered] = one_dark_yellow();
    colors[ImGuiCol_TableHeaderBg] = surface_panel();
    colors[ImGuiCol_TableBorderStrong] = one_dark_border();
    colors[ImGuiCol_TableBorderLight] =
        with_alpha(one_dark_border(), 0.55F);
    colors[ImGuiCol_TableRowBg] = rgba(0, 0, 0, 0);
    colors[ImGuiCol_TableRowBgAlt] =
        with_alpha(surface_panel(), 0.32F);
    colors[ImGuiCol_TextLink] = one_dark_blue();
    colors[ImGuiCol_TextSelectedBg] =
        with_alpha(one_dark_blue(), 0.32F);
    colors[ImGuiCol_TreeLines] = one_dark_comment();
    colors[ImGuiCol_DragDropTarget] = one_dark_cyan();
    colors[ImGuiCol_NavCursor] = one_dark_blue();
    colors[ImGuiCol_NavWindowingHighlight] =
        with_alpha(one_dark_blue(), 0.70F);
    colors[ImGuiCol_NavWindowingDimBg] =
        rgba(24, 26, 31, 170);
    colors[ImGuiCol_ModalWindowDimBg] =
        rgba(24, 26, 31, 190);

    if ((io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0) {
        style.WindowRounding = 0.0F;
        colors[ImGuiCol_WindowBg].w = 1.0F;
    }
}

void EditorUiKit::shutdown() noexcept {
    body_font_ = nullptr;
}

void EditorUiKit::brand_lockup() const {
    const float height = 35.0F;
    const float width = 178.0F;
    const ImVec2 minimum = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(width, height));
    ImDrawList& draw_list = *ImGui::GetWindowDrawList();

    const ImVec2 mark_minimum = add(minimum, 1.0F, 2.0F);
    draw_list.AddRectFilled(
        mark_minimum,
        add(mark_minimum, 31.0F, 31.0F),
        packed(surface_panel()),
        7.0F);
    draw_icon(
        draw_list,
        EditorIcon::brand,
        add(mark_minimum, 6.0F, 6.0F),
        19.0F,
        packed(one_dark_blue()),
        1.8F);
    draw_list.AddCircleFilled(
        add(mark_minimum, 25.0F, 7.0F),
        2.2F,
        packed(one_dark_purple()));

    ImFont* const font =
        body_font_ != nullptr ? body_font_ : ImGui::GetFont();
    draw_list.AddText(
        font,
        16.0F,
        add(minimum, 42.0F, 2.0F),
        packed(one_dark_bright()),
        "VULKAN CUBE");
    draw_list.AddText(
        font,
        11.5F,
        add(minimum, 42.0F, 20.0F),
        packed(one_dark_blue()),
        "EDITOR  /  REAL-TIME");
}

void EditorUiKit::panel_header(
    const EditorIcon icon,
    const char* const title,
    const char* const description) const {
    const float height =
        description != nullptr && description[0] != '\0'
        ? 43.0F
        : 31.0F;
    const ImVec2 minimum = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    ImGui::Dummy(ImVec2(width, height));

    ImDrawList& draw_list = *ImGui::GetWindowDrawList();
    draw_list.AddRectFilled(
        minimum,
        add(minimum, 3.0F, height - 7.0F),
        packed(one_dark_blue()),
        2.0F);
    draw_icon(
        draw_list,
        icon,
        add(minimum, 11.0F, 2.0F),
        17.0F,
        packed(one_dark_cyan()));
    ImFont* const font =
        body_font_ != nullptr ? body_font_ : ImGui::GetFont();
    draw_list.AddText(
        font,
        15.5F,
        add(minimum, 37.0F, 1.0F),
        packed(one_dark_bright()),
        title);
    if (description != nullptr && description[0] != '\0') {
        draw_list.AddText(
            font,
            12.5F,
            add(minimum, 37.0F, 21.0F),
            packed(one_dark_muted()),
            description);
    }
    draw_list.AddLine(
        add(minimum, 0.0F, height - 1.0F),
        add(minimum, width, height - 1.0F),
        packed(one_dark_border(), 0.68F),
        1.0F);
}

void EditorUiKit::section_header(
    const EditorIcon icon,
    const char* const label) const {
    const float height = 27.0F;
    const ImVec2 minimum = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const ImVec2 text_size = ImGui::CalcTextSize(label);
    ImGui::Dummy(ImVec2(width, height));

    ImDrawList& draw_list = *ImGui::GetWindowDrawList();
    draw_icon(
        draw_list,
        icon,
        add(minimum, 1.0F, 4.0F),
        15.0F,
        packed(one_dark_muted()),
        1.5F);
    draw_list.AddText(
        add(minimum, 23.0F, 3.0F),
        packed(one_dark_bright()),
        label);
    const float line_start =
        std::min(width, 31.0F + text_size.x);
    draw_list.AddLine(
        add(minimum, line_start, 12.0F),
        add(minimum, width, 12.0F),
        packed(one_dark_border(), 0.72F),
        1.0F);
}

bool EditorUiKit::action_button(
    const char* const id,
    const EditorIcon icon,
    const char* const label,
    const EditorButtonStyle button_style,
    const bool enabled,
    const bool active,
    ImVec2 size) const {
    const float height = std::max(
        ImGui::GetFrameHeight() + 2.0F, 30.0F);
    const float icon_size = 15.0F;
    const bool has_label = label != nullptr && label[0] != '\0';
    const ImVec2 text_size =
        has_label ? ImGui::CalcTextSize(label) : ImVec2{};
    if (size.x <= 0.0F) {
        size.x = has_label
            ? 11.0F + icon_size + 7.0F + text_size.x + 11.0F
            : height;
    }
    if (size.y <= 0.0F) {
        size.y = height;
    }

    ImGui::PushID(id);
    if (!enabled) {
        ImGui::BeginDisabled();
    }
    const bool activated =
        ImGui::InvisibleButton("##button", size);
    const bool hovered = enabled && ImGui::IsItemHovered();
    const bool held = enabled && ImGui::IsItemActive();
    const bool focused = enabled && ImGui::IsItemFocused();
    const ImVec2 minimum = ImGui::GetItemRectMin();
    const ImVec2 maximum = ImGui::GetItemRectMax();
    if (!enabled) {
        ImGui::EndDisabled();
    }
    ImGui::PopID();

    ImVec4 background = surface_raised();
    ImVec4 foreground = one_dark_text();
    ImVec4 border = one_dark_border();
    switch (button_style) {
    case EditorButtonStyle::quiet:
        background = rgba(0, 0, 0, 0);
        border = rgba(0, 0, 0, 0);
        break;
    case EditorButtonStyle::secondary:
        break;
    case EditorButtonStyle::primary:
        background =
            mix(surface_panel(), one_dark_blue(), 0.38F);
        foreground = one_dark_bright();
        border = mix(one_dark_border(), one_dark_blue(), 0.52F);
        break;
    case EditorButtonStyle::danger:
        background =
            mix(surface_panel(), one_dark_red(), 0.26F);
        foreground = one_dark_red();
        border = mix(one_dark_border(), one_dark_red(), 0.48F);
        break;
    }
    if (active) {
        background =
            mix(surface_panel(), one_dark_blue(), 0.28F);
        foreground = one_dark_blue();
        border = one_dark_blue();
    }
    if (hovered) {
        background = mix(background, one_dark_blue(), 0.16F);
        border = mix(border, one_dark_blue(), 0.42F);
    }
    if (held) {
        background = mix(background, one_dark_blue(), 0.28F);
    }
    if (focused) {
        border = one_dark_cyan();
    }
    const float alpha = enabled ? 1.0F : 0.42F;

    ImDrawList& draw_list = *ImGui::GetWindowDrawList();
    draw_list.AddRectFilled(
        minimum,
        maximum,
        packed(background, alpha),
        5.0F);
    if (border.w > 0.0F) {
        draw_list.AddRect(
            minimum,
            maximum,
            packed(border, alpha),
            5.0F,
            0,
            1.0F);
    }
    const float content_width = has_label
        ? icon_size + 7.0F + text_size.x
        : icon_size;
    const float content_x =
        minimum.x + (size.x - content_width) * 0.5F;
    const float icon_y =
        minimum.y + (size.y - icon_size) * 0.5F;
    draw_icon(
        draw_list,
        icon,
        ImVec2(content_x, icon_y),
        icon_size,
        packed(foreground, alpha),
        1.65F);
    if (has_label) {
        draw_list.AddText(
            ImVec2(
                content_x + icon_size + 7.0F,
                minimum.y + (size.y - text_size.y) * 0.5F),
            packed(foreground, alpha),
            label);
    }
    return enabled && activated;
}

bool EditorUiKit::icon_button(
    const char* const id,
    const EditorIcon icon,
    const char* const tooltip,
    const EditorButtonStyle button_style,
    const bool enabled,
    const bool active) const {
    const bool clicked = action_button(
        id,
        icon,
        "",
        button_style,
        enabled,
        active);
    if (tooltip != nullptr &&
        tooltip[0] != '\0' &&
        ImGui::IsItemHovered(
            ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", tooltip);
    }
    return clicked;
}

void EditorUiKit::badge(
    const char* const label,
    const EditorTone tone) const {
    const ImVec2 text_size = ImGui::CalcTextSize(label);
    const ImVec2 size(text_size.x + 15.0F, 20.0F);
    const ImVec2 minimum = ImGui::GetCursorScreenPos();
    ImGui::Dummy(size);

    const ImVec4 accent = color(tone);
    ImDrawList& draw_list = *ImGui::GetWindowDrawList();
    draw_list.AddRectFilled(
        minimum,
        add(minimum, size.x, size.y),
        packed(mix(surface_panel(), accent, 0.14F)),
        10.0F);
    draw_list.AddRect(
        minimum,
        add(minimum, size.x, size.y),
        packed(with_alpha(accent, 0.54F)),
        10.0F);
    draw_list.AddText(
        add(minimum, 7.5F, 2.0F),
        packed(accent),
        label);
}

void EditorUiKit::status_text(
    const char* const text,
    const EditorTone tone) const {
    ImGui::PushStyleColor(ImGuiCol_Text, color(tone));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

void EditorUiKit::empty_state(
    const EditorIcon icon,
    const char* const title,
    const char* const detail) const {
    const float width = ImGui::GetContentRegionAvail().x;
    const ImVec2 minimum = ImGui::GetCursorScreenPos();
    const float height = 86.0F;
    ImGui::Dummy(ImVec2(width, height));

    ImDrawList& draw_list = *ImGui::GetWindowDrawList();
    const float center_x = minimum.x + width * 0.5F;
    draw_list.AddCircleFilled(
        ImVec2(center_x, minimum.y + 23.0F),
        18.0F,
        packed(surface_panel()));
    draw_icon(
        draw_list,
        icon,
        ImVec2(center_x - 8.0F, minimum.y + 15.0F),
        16.0F,
        packed(one_dark_muted()));
    const ImVec2 title_size = ImGui::CalcTextSize(title);
    draw_list.AddText(
        ImVec2(
            center_x - title_size.x * 0.5F,
            minimum.y + 47.0F),
        packed(one_dark_bright()),
        title);
    const ImVec2 detail_size = ImGui::CalcTextSize(detail);
    draw_list.AddText(
        ImVec2(
            center_x - detail_size.x * 0.5F,
            minimum.y + 66.0F),
        packed(one_dark_muted()),
        detail);
}

EditorTreeRowResult EditorUiKit::hierarchy_row(
    const char* const id,
    const EditorIcon icon,
    const char* const label,
    const std::uint32_t depth,
    const bool has_children,
    const bool expanded,
    const bool selected,
    const bool visible) const {
    constexpr float row_height = 27.0F;
    constexpr float indent_width = 18.0F;
    const float width = ImGui::GetContentRegionAvail().x;
    ImGui::PushID(id);
    const bool activated = ImGui::InvisibleButton(
        "##hierarchy-row", ImVec2(width, row_height));
    const bool hovered = ImGui::IsItemHovered();
    const bool mouse_clicked = ImGui::IsItemClicked();
    const bool focused = ImGui::IsItemFocused();
    const ImVec2 minimum = ImGui::GetItemRectMin();
    const ImVec2 maximum = ImGui::GetItemRectMax();
    ImGui::PopID();

    ImDrawList& draw_list = *ImGui::GetWindowDrawList();
    if (selected || hovered) {
        const ImVec4 background = selected
            ? mix(surface_panel(), one_dark_blue(), 0.18F)
            : with_alpha(surface_panel(), 0.72F);
        draw_list.AddRectFilled(
            minimum, maximum, packed(background), 4.0F);
    }
    if (selected) {
        draw_list.AddRectFilled(
            minimum,
            ImVec2(minimum.x + 2.0F, maximum.y),
            packed(one_dark_blue()),
            2.0F);
    }
    if (focused) {
        draw_list.AddRect(
            minimum,
            maximum,
            packed(one_dark_cyan(), 0.78F),
            4.0F);
    }

    const std::uint32_t bounded_depth =
        std::min(depth, 32U);
    for (std::uint32_t level = 0U;
         level < bounded_depth;
         ++level) {
        const float guide_x = minimum.x + 8.0F +
            static_cast<float>(level) * indent_width;
        draw_list.AddLine(
            ImVec2(guide_x, minimum.y),
            ImVec2(guide_x, maximum.y),
            packed(one_dark_border(), 0.44F));
    }

    const float content_x = minimum.x + 5.0F +
        static_cast<float>(bounded_depth) * indent_width;
    const ImVec2 chevron_minimum(
        content_x,
        minimum.y + 5.0F);
    const ImVec2 chevron_maximum(
        content_x + 16.0F,
        minimum.y + 22.0F);
    if (has_children) {
        const ImU32 chevron_color = packed(
            hovered ? one_dark_text() : one_dark_muted());
        if (expanded) {
            draw_list.AddTriangleFilled(
                add(chevron_minimum, 3.0F, 5.0F),
                add(chevron_minimum, 12.0F, 5.0F),
                add(chevron_minimum, 7.5F, 10.0F),
                chevron_color);
        } else {
            draw_list.AddTriangleFilled(
                add(chevron_minimum, 5.0F, 3.0F),
                add(chevron_minimum, 10.0F, 7.5F),
                add(chevron_minimum, 5.0F, 12.0F),
                chevron_color);
        }
    }

    const ImVec4 icon_color =
        icon == EditorIcon::folder
        ? one_dark_yellow()
        : icon == EditorIcon::light
        ? one_dark_yellow()
        : icon == EditorIcon::environment
        ? one_dark_cyan()
        : icon == EditorIcon::camera
        ? one_dark_purple()
        : one_dark_blue();
    draw_icon(
        draw_list,
        icon,
        ImVec2(content_x + 19.0F, minimum.y + 5.0F),
        16.0F,
        packed(icon_color, visible ? 1.0F : 0.44F),
        1.55F);
    const ImVec2 label_minimum(
        content_x + 42.0F,
        minimum.y + 4.0F);
    const float label_maximum_x =
        std::max(label_minimum.x, maximum.x - 18.0F);
    draw_list.PushClipRect(
        ImVec2(label_minimum.x, minimum.y),
        ImVec2(label_maximum_x, maximum.y),
        true);
    draw_list.AddText(
        label_minimum,
        packed(
            selected ? one_dark_bright() : one_dark_text(),
            visible ? 1.0F : 0.48F),
        label);
    draw_list.PopClipRect();
    draw_list.AddCircleFilled(
        ImVec2(maximum.x - 9.0F, minimum.y + row_height * 0.5F),
        2.2F,
        packed(
            visible ? one_dark_green() : one_dark_comment(),
            visible ? 0.86F : 0.55F));

    EditorTreeRowResult result{};
    if (activated) {
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        if (mouse_clicked &&
            has_children &&
            point_inside(mouse, chevron_minimum, chevron_maximum)) {
            result.expansion_toggled = true;
        } else {
            result.selected = true;
        }
    }
    if (hovered &&
        ImGui::CalcTextSize(label).x >
            label_maximum_x - label_minimum.x) {
        ImGui::SetTooltip("%s", label);
    }
    return result;
}

EditorAssetRowResult EditorUiKit::asset_row(
    const char* const id,
    const EditorIcon icon,
    const char* const label,
    const char* const type_label,
    const bool selected) const {
    constexpr float row_height = 31.0F;
    const float width = ImGui::GetContentRegionAvail().x;
    ImGui::PushID(id);
    const bool activated = ImGui::InvisibleButton(
        "##asset-row", ImVec2(width, row_height));
    const bool hovered = ImGui::IsItemHovered();
    const bool double_clicked =
        hovered &&
        ImGui::IsMouseDoubleClicked(
            ImGuiMouseButton_Left);
    const bool focused = ImGui::IsItemFocused();
    const ImVec2 minimum = ImGui::GetItemRectMin();
    const ImVec2 maximum = ImGui::GetItemRectMax();
    ImGui::PopID();

    ImDrawList& draw_list = *ImGui::GetWindowDrawList();
    if (selected || hovered) {
        draw_list.AddRectFilled(
            minimum,
            maximum,
            packed(
                selected
                    ? mix(surface_panel(), one_dark_blue(), 0.18F)
                    : with_alpha(surface_panel(), 0.68F)),
            4.0F);
    }
    if (selected) {
        draw_list.AddRectFilled(
            minimum,
            ImVec2(minimum.x + 2.0F, maximum.y),
            packed(one_dark_blue()),
            2.0F);
    }
    if (focused) {
        draw_list.AddRect(
            minimum,
            maximum,
            packed(one_dark_cyan(), 0.78F),
            4.0F);
    }
    const ImVec4 icon_color =
        icon == EditorIcon::folder
        ? one_dark_yellow()
        : icon == EditorIcon::material
        ? one_dark_purple()
        : icon == EditorIcon::model
        ? one_dark_blue()
        : one_dark_cyan();
    draw_icon(
        draw_list,
        icon,
        add(minimum, 9.0F, 7.0F),
        17.0F,
        packed(icon_color),
        1.6F);
    const ImVec2 label_minimum = add(minimum, 35.0F, 6.0F);
    const ImVec2 type_size = ImGui::CalcTextSize(type_label);
    const float type_minimum_x =
        maximum.x - type_size.x - 9.0F;
    const float label_maximum_x =
        std::max(label_minimum.x, type_minimum_x - 8.0F);
    draw_list.PushClipRect(
        ImVec2(label_minimum.x, minimum.y),
        ImVec2(label_maximum_x, maximum.y),
        true);
    draw_list.AddText(
        label_minimum,
        packed(selected ? one_dark_bright() : one_dark_text()),
        label);
    draw_list.PopClipRect();
    draw_list.AddText(
        ImVec2(
            type_minimum_x,
            minimum.y + 6.0F),
        packed(one_dark_muted()),
        type_label);
    if (hovered &&
        ImGui::CalcTextSize(label).x >
            label_maximum_x - label_minimum.x) {
        ImGui::SetTooltip("%s", label);
    }
    return EditorAssetRowResult{
        .activated = activated || double_clicked,
        .double_clicked = double_clicked,
    };
}

bool EditorUiKit::begin_property_grid(
    const char* const id) const {
    const ImGuiTableFlags flags =
        ImGuiTableFlags_SizingStretchProp |
        ImGuiTableFlags_NoSavedSettings |
        ImGuiTableFlags_PadOuterX |
        ImGuiTableFlags_BordersInnerH;
    if (!ImGui::BeginTable(id, 2, flags)) {
        return false;
    }
    ImGui::TableSetupColumn(
        "Property",
        ImGuiTableColumnFlags_WidthFixed,
        88.0F);
    ImGui::TableSetupColumn(
        "Value",
        ImGuiTableColumnFlags_WidthStretch);
    return true;
}

void EditorUiKit::property_row(
    const char* const label) const {
    ImGui::TableNextRow(ImGuiTableRowFlags_None, 31.0F);
    static_cast<void>(ImGui::TableSetColumnIndex(0));
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(one_dark_muted(), "%s", label);
    static_cast<void>(ImGui::TableSetColumnIndex(1));
    ImGui::SetNextItemWidth(-1.0F);
}

void EditorUiKit::end_property_grid() const {
    ImGui::EndTable();
}

bool EditorUiKit::drag_vec3(
    const char* const id,
    const char* const label,
    float* const values,
    const float speed,
    const float reset_value,
    const float minimum,
    const float maximum) const {
    property_row(label);
    ImGui::PushID(id);

    const float available = ImGui::GetContentRegionAvail().x;
    const float outer_spacing = ImGui::GetStyle().ItemSpacing.x;
    const float inner_spacing = 2.0F;
    const float label_width = 18.0F;
    const float group_width =
        std::max(
            (available - outer_spacing * 2.0F) / 3.0F,
            34.0F);
    const float field_width =
        std::max(
            group_width - label_width - inner_spacing,
            16.0F);
    constexpr std::array<const char*, 3U> axis_labels{
        "X", "Y", "Z"};
    constexpr std::array<EditorTone, 3U> axis_tones{
        EditorTone::danger,
        EditorTone::success,
        EditorTone::primary,
    };

    bool changed = false;
    for (std::size_t axis = 0U; axis < axis_labels.size(); ++axis) {
        ImGui::PushID(static_cast<int>(axis));
        const ImVec4 accent = color(axis_tones[axis]);
        ImGui::PushStyleColor(
            ImGuiCol_Button,
            with_alpha(accent, 0.24F));
        ImGui::PushStyleColor(
            ImGuiCol_ButtonHovered,
            with_alpha(accent, 0.42F));
        ImGui::PushStyleColor(
            ImGuiCol_ButtonActive,
            with_alpha(accent, 0.58F));
        ImGui::PushStyleColor(ImGuiCol_Text, accent);
        if (ImGui::Button(
                axis_labels[axis],
                ImVec2(label_width, 0.0F))) {
            if (values[axis] != reset_value) {
                values[axis] = reset_value;
                changed = true;
            }
        }
        ImGui::PopStyleColor(4);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Reset %s to %.2f",
                axis_labels[axis],
                static_cast<double>(reset_value));
        }
        ImGui::SameLine(0.0F, inner_spacing);
        ImGui::SetNextItemWidth(field_width);
        changed |= ImGui::DragFloat(
            "##value",
            &values[axis],
            speed,
            minimum,
            maximum,
            "%.2f");
        ImGui::PopID();
        if (axis + 1U < axis_labels.size()) {
            ImGui::SameLine(0.0F, outer_spacing);
        }
    }
    ImGui::PopID();
    return changed;
}

void EditorUiKit::metric_card(
    const char* const id,
    const char* const label,
    const char* const value,
    const EditorTone tone) const {
    const float width = ImGui::GetContentRegionAvail().x;
    constexpr float height = 57.0F;
    ImGui::PushID(id);
    ImGui::Dummy(ImVec2(width, height));
    const ImVec2 minimum = ImGui::GetItemRectMin();
    const ImVec2 maximum = ImGui::GetItemRectMax();
    ImGui::PopID();

    const ImVec4 accent = color(tone);
    ImDrawList& draw_list = *ImGui::GetWindowDrawList();
    draw_list.AddRectFilled(
        minimum,
        maximum,
        packed(surface_panel()),
        5.0F);
    draw_list.AddRect(
        minimum,
        maximum,
        packed(one_dark_border()),
        5.0F);
    draw_list.AddRectFilled(
        minimum,
        ImVec2(minimum.x + 3.0F, maximum.y),
        packed(accent),
        5.0F);
    draw_list.AddText(
        add(minimum, 11.0F, 7.0F),
        packed(one_dark_muted()),
        label);
    ImFont* const font =
        body_font_ != nullptr ? body_font_ : ImGui::GetFont();
    draw_list.AddText(
        font,
        18.0F,
        add(minimum, 11.0F, 27.0F),
        packed(accent),
        value);
}

void EditorUiKit::viewport_badge(
    const ImVec2 position,
    const char* const text,
    const EditorTone tone) const {
    const ImVec2 text_size = ImGui::CalcTextSize(text);
    const ImVec2 maximum(
        position.x + text_size.x + 29.0F,
        position.y + 24.0F);
    const ImVec4 accent = color(tone);
    ImDrawList& draw_list = *ImGui::GetWindowDrawList();
    draw_list.AddRectFilled(
        position,
        maximum,
        packed(rgba(24, 26, 31, 224)),
        5.0F);
    draw_list.AddRect(
        position,
        maximum,
        packed(with_alpha(accent, 0.56F)),
        5.0F);
    draw_list.AddCircleFilled(
        ImVec2(position.x + 10.0F, position.y + 12.0F),
        3.0F,
        packed(accent));
    draw_list.AddText(
        ImVec2(position.x + 18.0F, position.y + 3.0F),
        packed(one_dark_bright()),
        text);
}

ImVec4 EditorUiKit::color(
    const EditorTone tone) noexcept {
    switch (tone) {
    case EditorTone::neutral: return one_dark_muted();
    case EditorTone::primary: return one_dark_blue();
    case EditorTone::info: return one_dark_cyan();
    case EditorTone::success: return one_dark_green();
    case EditorTone::warning: return one_dark_yellow();
    case EditorTone::danger: return one_dark_red();
    case EditorTone::purple: return one_dark_purple();
    }
    return one_dark_text();
}

ImVec4 EditorUiKit::text_color() noexcept {
    return one_dark_text();
}

ImVec4 EditorUiKit::muted_text_color() noexcept {
    return one_dark_muted();
}

ImVec4 EditorUiKit::border_color() noexcept {
    return one_dark_border();
}

void EditorUiKit::draw_icon(
    ImDrawList& draw_list,
    const EditorIcon icon,
    const ImVec2 minimum,
    const float size,
    const ImU32 color_value,
    const float thickness) {
    const float left = minimum.x;
    const float top = minimum.y;
    const float right = left + size;
    const float bottom = top + size;
    const float center_x = left + size * 0.5F;
    const float center_y = top + size * 0.5F;
    const float unit = size / 16.0F;

    switch (icon) {
    case EditorIcon::brand:
    case EditorIcon::cube: {
        const ImVec2 top_point(center_x, top + 1.0F * unit);
        const ImVec2 left_point(left + 2.0F * unit, top + 5.0F * unit);
        const ImVec2 center_point(center_x, top + 9.0F * unit);
        const ImVec2 right_point(right - 2.0F * unit, top + 5.0F * unit);
        const ImVec2 bottom_point(center_x, bottom - 1.0F * unit);
        draw_list.AddLine(
            top_point, left_point, color_value, thickness);
        draw_list.AddLine(
            left_point, center_point, color_value, thickness);
        draw_list.AddLine(
            center_point, right_point, color_value, thickness);
        draw_list.AddLine(
            right_point, top_point, color_value, thickness);
        draw_list.AddLine(
            center_point, bottom_point, color_value, thickness);
        draw_list.AddLine(
            left_point, bottom_point, color_value, thickness);
        draw_list.AddLine(
            right_point, bottom_point, color_value, thickness);
        draw_list.AddLine(
            top_point, center_point, color_value, thickness);
        break;
    }
    case EditorIcon::play:
    case EditorIcon::resume:
        draw_list.AddTriangleFilled(
            ImVec2(left + 4.0F * unit, top + 2.0F * unit),
            ImVec2(right - 2.0F * unit, center_y),
            ImVec2(left + 4.0F * unit, bottom - 2.0F * unit),
            color_value);
        break;
    case EditorIcon::stop:
        draw_list.AddRectFilled(
            ImVec2(left + 3.0F * unit, top + 3.0F * unit),
            ImVec2(right - 3.0F * unit, bottom - 3.0F * unit),
            color_value,
            1.5F * unit);
        break;
    case EditorIcon::pause:
        draw_list.AddRectFilled(
            ImVec2(left + 3.0F * unit, top + 2.5F * unit),
            ImVec2(left + 6.5F * unit, bottom - 2.5F * unit),
            color_value,
            unit);
        draw_list.AddRectFilled(
            ImVec2(left + 9.5F * unit, top + 2.5F * unit),
            ImVec2(right - 3.0F * unit, bottom - 2.5F * unit),
            color_value,
            unit);
        break;
    case EditorIcon::external_window:
        draw_list.AddRect(
            ImVec2(left + 2.0F * unit, top + 5.0F * unit),
            ImVec2(right - 5.0F * unit, bottom - 2.0F * unit),
            color_value,
            unit,
            0,
            thickness);
        draw_list.AddLine(
            ImVec2(left + 7.0F * unit, top + 2.0F * unit),
            ImVec2(right - 2.0F * unit, top + 2.0F * unit),
            color_value,
            thickness);
        draw_list.AddLine(
            ImVec2(right - 2.0F * unit, top + 2.0F * unit),
            ImVec2(right - 2.0F * unit, top + 7.0F * unit),
            color_value,
            thickness);
        draw_list.AddLine(
            ImVec2(right - 2.0F * unit, top + 2.0F * unit),
            ImVec2(left + 7.0F * unit, top + 9.0F * unit),
            color_value,
            thickness);
        break;
    case EditorIcon::search:
        draw_list.AddCircle(
            ImVec2(left + 6.5F * unit, top + 6.5F * unit),
            4.0F * unit,
            color_value,
            16,
            thickness);
        draw_list.AddLine(
            ImVec2(left + 9.5F * unit, top + 9.5F * unit),
            ImVec2(right - 2.0F * unit, bottom - 2.0F * unit),
            color_value,
            thickness);
        break;
    case EditorIcon::folder:
        draw_list.AddRect(
            ImVec2(left + 1.5F * unit, top + 5.0F * unit),
            ImVec2(right - 1.5F * unit, bottom - 2.0F * unit),
            color_value,
            2.0F * unit,
            0,
            thickness);
        draw_list.AddLine(
            ImVec2(left + 2.0F * unit, top + 5.0F * unit),
            ImVec2(left + 5.0F * unit, top + 2.5F * unit),
            color_value,
            thickness);
        draw_list.AddLine(
            ImVec2(left + 5.0F * unit, top + 2.5F * unit),
            ImVec2(left + 9.0F * unit, top + 2.5F * unit),
            color_value,
            thickness);
        draw_list.AddLine(
            ImVec2(left + 9.0F * unit, top + 2.5F * unit),
            ImVec2(left + 11.0F * unit, top + 5.0F * unit),
            color_value,
            thickness);
        break;
    case EditorIcon::model:
        draw_icon(
            draw_list,
            EditorIcon::cube,
            minimum,
            size,
            color_value,
            thickness);
        draw_list.AddCircleFilled(
            ImVec2(right - 2.5F * unit, bottom - 2.5F * unit),
            1.6F * unit,
            color_value);
        break;
    case EditorIcon::ground:
        draw_list.AddLine(
            ImVec2(left + unit, center_y),
            ImVec2(right - unit, center_y),
            color_value,
            thickness);
        draw_list.AddLine(
            ImVec2(left + 3.0F * unit, center_y),
            ImVec2(left + 6.0F * unit, bottom - 2.0F * unit),
            color_value,
            thickness);
        draw_list.AddLine(
            ImVec2(right - 3.0F * unit, center_y),
            ImVec2(right - 6.0F * unit, bottom - 2.0F * unit),
            color_value,
            thickness);
        break;
    case EditorIcon::terrain:
        draw_list.AddPolyline(
            std::array<ImVec2, 4U>{
                ImVec2(left + unit, bottom - 2.0F * unit),
                ImVec2(left + 5.0F * unit, top + 5.0F * unit),
                ImVec2(left + 8.0F * unit, top + 9.0F * unit),
                ImVec2(right - unit, top + 3.0F * unit),
            }.data(),
            4,
            color_value,
            0,
            thickness);
        break;
    case EditorIcon::camera:
        draw_list.AddRect(
            ImVec2(left + 1.5F * unit, top + 4.0F * unit),
            ImVec2(right - 4.0F * unit, bottom - 3.0F * unit),
            color_value,
            2.0F * unit,
            0,
            thickness);
        draw_list.AddTriangleFilled(
            ImVec2(right - 4.0F * unit, top + 6.0F * unit),
            ImVec2(right - 1.0F * unit, top + 4.0F * unit),
            ImVec2(right - 1.0F * unit, bottom - 3.0F * unit),
            color_value);
        draw_list.AddCircle(
            ImVec2(left + 6.5F * unit, center_y),
            2.3F * unit,
            color_value,
            12,
            thickness);
        break;
    case EditorIcon::light:
        draw_list.AddCircle(
            ImVec2(center_x, center_y),
            3.0F * unit,
            color_value,
            16,
            thickness);
        for (int ray = 0; ray < 8; ++ray) {
            const float angle =
                static_cast<float>(ray) *
                3.14159265F * 0.25F;
            const ImVec2 inner(
                center_x + std::cos(angle) * 5.0F * unit,
                center_y + std::sin(angle) * 5.0F * unit);
            const ImVec2 outer(
                center_x + std::cos(angle) * 7.0F * unit,
                center_y + std::sin(angle) * 7.0F * unit);
            draw_list.AddLine(
                inner, outer, color_value, thickness);
        }
        break;
    case EditorIcon::environment:
        draw_list.AddCircle(
            ImVec2(center_x, center_y),
            6.0F * unit,
            color_value,
            20,
            thickness);
        draw_list.AddEllipse(
            ImVec2(center_x, center_y),
            ImVec2(3.0F * unit, 6.0F * unit),
            color_value,
            0.0F,
            20,
            thickness);
        draw_list.AddLine(
            ImVec2(left + 2.0F * unit, center_y),
            ImVec2(right - 2.0F * unit, center_y),
            color_value,
            thickness);
        break;
    case EditorIcon::refresh:
    case EditorIcon::revert:
        draw_list.PathArcTo(
            ImVec2(center_x, center_y),
            5.5F * unit,
            -2.6F,
            2.3F,
            20);
        draw_list.PathStroke(color_value, 0, thickness);
        draw_list.AddTriangleFilled(
            ImVec2(right - 3.0F * unit, top + 4.0F * unit),
            ImVec2(right - 2.0F * unit, top + 9.0F * unit),
            ImVec2(right - 7.0F * unit, top + 7.0F * unit),
            color_value);
        break;
    case EditorIcon::import_asset:
        draw_list.AddLine(
            ImVec2(center_x, top + 1.5F * unit),
            ImVec2(center_x, top + 10.0F * unit),
            color_value,
            thickness);
        draw_list.AddTriangleFilled(
            ImVec2(center_x, top + 12.0F * unit),
            ImVec2(left + 4.0F * unit, top + 7.0F * unit),
            ImVec2(right - 4.0F * unit, top + 7.0F * unit),
            color_value);
        draw_list.AddLine(
            ImVec2(left + 2.0F * unit, bottom - 2.0F * unit),
            ImVec2(right - 2.0F * unit, bottom - 2.0F * unit),
            color_value,
            thickness);
        break;
    case EditorIcon::add:
        draw_list.AddLine(
            ImVec2(center_x, top + 3.0F * unit),
            ImVec2(center_x, bottom - 3.0F * unit),
            color_value,
            thickness);
        draw_list.AddLine(
            ImVec2(left + 3.0F * unit, center_y),
            ImVec2(right - 3.0F * unit, center_y),
            color_value,
            thickness);
        break;
    case EditorIcon::save:
        draw_list.AddRect(
            ImVec2(left + 2.0F * unit, top + 1.5F * unit),
            ImVec2(right - 2.0F * unit, bottom - 1.5F * unit),
            color_value,
            1.5F * unit,
            0,
            thickness);
        draw_list.AddRect(
            ImVec2(left + 5.0F * unit, top + 2.0F * unit),
            ImVec2(right - 5.0F * unit, top + 6.0F * unit),
            color_value,
            0.0F,
            0,
            thickness);
        draw_list.AddRect(
            ImVec2(left + 5.0F * unit, top + 9.0F * unit),
            ImVec2(right - 5.0F * unit, bottom - 2.0F * unit),
            color_value,
            unit,
            0,
            thickness);
        break;
    case EditorIcon::renderer:
        draw_list.AddCircle(
            ImVec2(center_x, center_y + 1.0F * unit),
            6.0F * unit,
            color_value,
            20,
            thickness);
        draw_list.AddLine(
            ImVec2(center_x, center_y + 1.0F * unit),
            ImVec2(
                center_x + 3.8F * unit,
                center_y - 2.5F * unit),
            color_value,
            thickness);
        draw_list.AddCircleFilled(
            ImVec2(center_x, center_y + 1.0F * unit),
            1.4F * unit,
            color_value);
        break;
    case EditorIcon::material:
        draw_list.AddCircle(
            ImVec2(center_x, center_y),
            6.0F * unit,
            color_value,
            20,
            thickness);
        draw_list.AddCircleFilled(
            ImVec2(center_x - 2.0F * unit, center_y - 2.0F * unit),
            1.2F * unit,
            color_value);
        draw_list.AddLine(
            ImVec2(center_x, center_y),
            ImVec2(right - 2.0F * unit, bottom - 2.0F * unit),
            color_value,
            thickness);
        break;
    case EditorIcon::eye:
        draw_list.AddBezierCubic(
            ImVec2(left + unit, center_y),
            ImVec2(left + 4.0F * unit, top + 2.0F * unit),
            ImVec2(right - 4.0F * unit, top + 2.0F * unit),
            ImVec2(right - unit, center_y),
            color_value,
            thickness);
        draw_list.AddBezierCubic(
            ImVec2(left + unit, center_y),
            ImVec2(left + 4.0F * unit, bottom - 2.0F * unit),
            ImVec2(right - 4.0F * unit, bottom - 2.0F * unit),
            ImVec2(right - unit, center_y),
            color_value,
            thickness);
        draw_list.AddCircleFilled(
            ImVec2(center_x, center_y),
            2.0F * unit,
            color_value);
        break;
    }
}

} // namespace vcr
