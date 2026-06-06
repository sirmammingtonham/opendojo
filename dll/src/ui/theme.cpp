#include "ui/theme.hpp"

#include "imgui.h"

namespace opendojo::theme {

namespace {

ImVec4 rgba(int r, int g, int b, float a = 1.0f) {
    return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a);
}

}  // namespace

void apply() {
    auto& style = ImGui::GetStyle();
    auto& c = style.Colors;

    // Sharp edges, dense layout. Tekken's UI is angular, not friendly.
    style.WindowRounding = 0.0f;
    style.ChildRounding = 0.0f;
    style.FrameRounding = 0.0f;
    style.PopupRounding = 0.0f;
    style.ScrollbarRounding = 0.0f;
    style.GrabRounding = 0.0f;
    style.TabRounding = 0.0f;

    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.TabBorderSize = 0.0f;

    style.WindowPadding = ImVec2(14, 12);
    style.FramePadding = ImVec2(8, 5);
    style.ItemSpacing = ImVec2(8, 6);
    style.ItemInnerSpacing = ImVec2(6, 4);
    style.IndentSpacing = 18.0f;
    style.ScrollbarSize = 14.0f;
    style.GrabMinSize = 12.0f;

    // --- Color palette ----------------------------------------------------
    // Background: near-black with a hint of warmth.
    const ImVec4 bg_dark = rgba(14, 12, 14, 0.96f);
    const ImVec4 bg_mid = rgba(24, 22, 24);
    const ImVec4 bg_light = rgba(36, 32, 34);
    const ImVec4 border = rgba(64, 58, 60);
    const ImVec4 text_main = rgba(232, 226, 220);
    const ImVec4 text_dim = rgba(150, 140, 134);
    const ImVec4 text_off = rgba(96, 88, 84);

    // Crimson accent and its hover/active variants.
    const ImVec4 accent = ImVec4(ACCENT_R, ACCENT_G, ACCENT_B, 1.0f);
    const ImVec4 accent_hi = ImVec4(ACCENT_R + 0.10f, ACCENT_G + 0.06f, ACCENT_B + 0.06f, 1.0f);
    const ImVec4 accent_dn = ImVec4(ACCENT_R - 0.10f, ACCENT_G, ACCENT_B, 1.0f);
    const ImVec4 accent_dim = ImVec4(ACCENT_R, ACCENT_G, ACCENT_B, 0.30f);

    c[ImGuiCol_Text] = text_main;
    c[ImGuiCol_TextDisabled] = text_off;

    c[ImGuiCol_WindowBg] = bg_dark;
    c[ImGuiCol_ChildBg] = bg_mid;
    c[ImGuiCol_PopupBg] = bg_dark;

    c[ImGuiCol_Border] = border;
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);

    c[ImGuiCol_FrameBg] = bg_mid;
    c[ImGuiCol_FrameBgHovered] = bg_light;
    c[ImGuiCol_FrameBgActive] = bg_light;

    c[ImGuiCol_TitleBg] = bg_mid;
    c[ImGuiCol_TitleBgActive] = accent;
    c[ImGuiCol_TitleBgCollapsed] = bg_mid;

    c[ImGuiCol_MenuBarBg] = bg_mid;

    c[ImGuiCol_ScrollbarBg] = bg_dark;
    c[ImGuiCol_ScrollbarGrab] = bg_light;
    c[ImGuiCol_ScrollbarGrabHovered] = accent_dim;
    c[ImGuiCol_ScrollbarGrabActive] = accent;

    c[ImGuiCol_CheckMark] = accent_hi;
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = accent_hi;

    c[ImGuiCol_Button] = bg_light;
    c[ImGuiCol_ButtonHovered] = accent;
    c[ImGuiCol_ButtonActive] = accent_dn;

    c[ImGuiCol_Header] = accent_dim;
    c[ImGuiCol_HeaderHovered] = accent;
    c[ImGuiCol_HeaderActive] = accent_dn;

    c[ImGuiCol_Separator] = border;
    c[ImGuiCol_SeparatorHovered] = accent;
    c[ImGuiCol_SeparatorActive] = accent_hi;

    c[ImGuiCol_ResizeGrip] = accent_dim;
    c[ImGuiCol_ResizeGripHovered] = accent;
    c[ImGuiCol_ResizeGripActive] = accent_hi;

    c[ImGuiCol_Tab] = bg_mid;
    c[ImGuiCol_TabHovered] = accent;
    c[ImGuiCol_TabActive] = accent_dn;
    c[ImGuiCol_TabUnfocused] = bg_mid;
    c[ImGuiCol_TabUnfocusedActive] = bg_light;

    c[ImGuiCol_PlotLines] = text_dim;
    c[ImGuiCol_PlotLinesHovered] = accent_hi;
    c[ImGuiCol_PlotHistogram] = accent;
    c[ImGuiCol_PlotHistogramHovered] = accent_hi;

    c[ImGuiCol_TableHeaderBg] = bg_mid;
    c[ImGuiCol_TableBorderStrong] = border;
    c[ImGuiCol_TableBorderLight] = ImVec4(border.x, border.y, border.z, 0.45f);
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.02f);

    c[ImGuiCol_TextSelectedBg] = accent_dim;
    c[ImGuiCol_DragDropTarget] = accent_hi;
    c[ImGuiCol_NavHighlight] = accent_hi;
    c[ImGuiCol_NavWindowingHighlight] = accent_hi;
    c[ImGuiCol_NavWindowingDimBg] = ImVec4(0, 0, 0, 0.6f);
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.6f);
}

}  // namespace opendojo::theme
