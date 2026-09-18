#pragma once

#include "imgui.h"

namespace opendojo::ui {
// Selectable extends its hit/highlight rectangle by half ItemSpacing.
// Match that extension to the cell padding so the entire cell is the action,
// without drawing a second bordered box inside it.
inline bool cell_action(const char* label, float content_height = 0.0f, bool destructive = false) {
    const auto padding = ImGui::GetStyle().CellPadding;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(padding.x * 2, padding.y * 2));
    ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.5f, 0.5f));
    if (destructive) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.40f, 1));
    const bool clicked = ImGui::Selectable(
        label, false, 0,
        ImVec2(0, content_height > 0 ? content_height : ImGui::GetTextLineHeight()));
    if (destructive) ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    return clicked;
}
}  // namespace opendojo::ui
