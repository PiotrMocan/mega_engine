#pragma once

#include "studio_brush_history.hpp"

#include <imgui.h>

namespace vcr {

template<class Sample>
[[nodiscard]] bool draw_studio_brush_controls(StudioBrushHistory<Sample>& history,
    std::vector<Sample>& layer, const char* clear_label) {
    const bool keyboard = !ImGui::GetIO().WantTextInput && !ImGui::IsAnyItemActive() &&
        !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup);
    ImGui::BeginDisabled(!history.can_undo(layer));
    const bool undo = ImGui::SmallButton("Undo Stroke");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!history.can_redo());
    const bool redo = ImGui::SmallButton("Redo Stroke");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(layer.empty());
    const bool clear = ImGui::SmallButton(clear_label);
    ImGui::EndDisabled();
    if (undo || (keyboard && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z, ImGuiInputFlags_RouteGlobal)))
        return history.undo(layer);
    if (redo || (keyboard && (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Y, ImGuiInputFlags_RouteGlobal) ||
            ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z, ImGuiInputFlags_RouteGlobal))))
        return history.redo(layer);
    return clear && history.clear(layer);
}

} // namespace vcr
