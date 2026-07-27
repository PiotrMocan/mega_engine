#pragma once

#include <imgui.h>

#include <cstdint>

namespace vcr {

enum class EditorIcon : std::uint8_t {
    brand,
    play,
    stop,
    pause,
    resume,
    external_window,
    search,
    folder,
    cube,
    model,
    ground,
    terrain,
    camera,
    light,
    environment,
    refresh,
    import_asset,
    add,
    save,
    revert,
    renderer,
    material,
    eye,
};

enum class EditorTone : std::uint8_t {
    neutral,
    primary,
    info,
    success,
    warning,
    danger,
    purple,
};

enum class EditorButtonStyle : std::uint8_t {
    quiet,
    secondary,
    primary,
    danger,
};

struct EditorTreeRowResult {
    bool selected = false;
    bool expansion_toggled = false;
};

class EditorUiKit final {
public:
    void initialize();
    void shutdown() noexcept;

    void brand_lockup() const;
    void panel_header(
        EditorIcon icon,
        const char* title,
        const char* description = nullptr) const;
    void section_header(
        EditorIcon icon,
        const char* label) const;

    [[nodiscard]] bool action_button(
        const char* id,
        EditorIcon icon,
        const char* label,
        EditorButtonStyle style =
            EditorButtonStyle::secondary,
        bool enabled = true,
        bool active = false,
        ImVec2 size = ImVec2{}) const;
    [[nodiscard]] bool icon_button(
        const char* id,
        EditorIcon icon,
        const char* tooltip,
        EditorButtonStyle style =
            EditorButtonStyle::quiet,
        bool enabled = true,
        bool active = false) const;

    void badge(
        const char* label,
        EditorTone tone = EditorTone::neutral) const;
    void status_text(
        const char* text,
        EditorTone tone = EditorTone::neutral) const;
    void empty_state(
        EditorIcon icon,
        const char* title,
        const char* detail) const;

    [[nodiscard]] EditorTreeRowResult hierarchy_row(
        const char* id,
        EditorIcon icon,
        const char* label,
        std::uint32_t depth,
        bool has_children,
        bool expanded,
        bool selected,
        bool visible) const;
    [[nodiscard]] bool asset_row(
        const char* id,
        EditorIcon icon,
        const char* label,
        const char* type_label,
        bool selected) const;

    [[nodiscard]] bool begin_property_grid(
        const char* id) const;
    void property_row(const char* label) const;
    void end_property_grid() const;
    [[nodiscard]] bool drag_vec3(
        const char* id,
        const char* label,
        float* values,
        float speed,
        float reset_value,
        float minimum = 0.0F,
        float maximum = 0.0F) const;

    void metric_card(
        const char* id,
        const char* label,
        const char* value,
        EditorTone tone = EditorTone::neutral) const;
    void viewport_badge(
        ImVec2 position,
        const char* text,
        EditorTone tone = EditorTone::neutral) const;

    [[nodiscard]] static ImVec4 color(
        EditorTone tone) noexcept;
    [[nodiscard]] static ImVec4 text_color() noexcept;
    [[nodiscard]] static ImVec4 muted_text_color() noexcept;
    [[nodiscard]] static ImVec4 border_color() noexcept;

private:
    static void draw_icon(
        ImDrawList& draw_list,
        EditorIcon icon,
        ImVec2 minimum,
        float size,
        ImU32 color,
        float thickness = 1.7F);

    ImFont* body_font_ = nullptr;
};

} // namespace vcr
