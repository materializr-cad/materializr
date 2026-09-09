#include "ThemeManager.h"
#include <imgui.h>
#include "../i18n.h"
#include "../i18n.h"

namespace materializr {

ThemeManager::ThemeManager() = default;

void ThemeManager::setTheme(Theme theme) {
    m_theme = theme;
    apply();
}

Theme ThemeManager::getTheme() const {
    return m_theme;
}

void ThemeManager::toggle() {
    if (m_theme == Theme::Dark) {
        setTheme(Theme::Light);
    } else {
        setTheme(Theme::Dark);
    }
}

void ThemeManager::apply() {
    if (m_theme == Theme::Dark) {
        applyDark();
    } else {
        applyLight();
    }
}

bool ThemeManager::renderSelector() {
    int current = static_cast<int>(m_theme);
    const char* items[] = { materializr::tr("Dark"), materializr::tr("Light") };

    if (ImGui::Combo(materializr::tr("Theme"), &current, items, 2)) {
        setTheme(static_cast<Theme>(current));
        return true;
    }

    return false;
}

void ThemeManager::applyDark() {
    ImGuiStyle& style = ImGui::GetStyle();

    // Rounding
    style.WindowRounding = 4.0f;
    style.FrameRounding = 2.0f;
    style.GrabRounding = 2.0f;

    ImVec4* colors = style.Colors;

    colors[ImGuiCol_WindowBg]         = ImVec4(0.12f, 0.12f, 0.14f, 1.0f);
    colors[ImGuiCol_ChildBg]          = ImVec4(0.12f, 0.12f, 0.14f, 0.0f);
    colors[ImGuiCol_PopupBg]          = ImVec4(0.10f, 0.10f, 0.12f, 0.94f);
    colors[ImGuiCol_Border]           = ImVec4(0.25f, 0.25f, 0.28f, 0.50f);
    colors[ImGuiCol_BorderShadow]     = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

    colors[ImGuiCol_TitleBg]          = ImVec4(0.08f, 0.08f, 0.10f, 1.0f);
    colors[ImGuiCol_TitleBgActive]    = ImVec4(0.12f, 0.12f, 0.16f, 1.0f);
    colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.08f, 0.08f, 0.10f, 0.50f);

    colors[ImGuiCol_MenuBarBg]        = ImVec4(0.14f, 0.14f, 0.16f, 1.0f);

    colors[ImGuiCol_FrameBg]          = ImVec4(0.18f, 0.18f, 0.22f, 1.0f);
    colors[ImGuiCol_FrameBgHovered]   = ImVec4(0.24f, 0.24f, 0.30f, 1.0f);
    colors[ImGuiCol_FrameBgActive]    = ImVec4(0.28f, 0.28f, 0.36f, 1.0f);

    colors[ImGuiCol_Button]           = ImVec4(0.22f, 0.22f, 0.28f, 1.0f);
    colors[ImGuiCol_ButtonHovered]    = ImVec4(0.30f, 0.30f, 0.38f, 1.0f);
    colors[ImGuiCol_ButtonActive]     = ImVec4(0.35f, 0.35f, 0.45f, 1.0f);

    colors[ImGuiCol_Header]           = ImVec4(0.22f, 0.28f, 0.38f, 1.0f);
    colors[ImGuiCol_HeaderHovered]    = ImVec4(0.28f, 0.35f, 0.48f, 1.0f);
    colors[ImGuiCol_HeaderActive]     = ImVec4(0.30f, 0.38f, 0.52f, 1.0f);

    colors[ImGuiCol_Tab]              = ImVec4(0.14f, 0.14f, 0.18f, 1.0f);
    colors[ImGuiCol_TabHovered]       = ImVec4(0.28f, 0.35f, 0.48f, 1.0f);
    colors[ImGuiCol_TabSelected]      = ImVec4(0.22f, 0.28f, 0.38f, 1.0f);
    colors[ImGuiCol_TabDimmed]         = ImVec4(0.11f, 0.11f, 0.14f, 1.0f);
    colors[ImGuiCol_TabDimmedSelected] = ImVec4(0.17f, 0.19f, 0.26f, 1.0f);

    colors[ImGuiCol_ScrollbarBg]      = ImVec4(0.10f, 0.10f, 0.12f, 0.50f);
    colors[ImGuiCol_ScrollbarGrab]    = ImVec4(0.30f, 0.30f, 0.34f, 1.0f);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.38f, 0.38f, 0.42f, 1.0f);
    colors[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.45f, 0.45f, 0.50f, 1.0f);

    colors[ImGuiCol_Separator]        = ImVec4(0.25f, 0.25f, 0.28f, 0.50f);
    colors[ImGuiCol_SeparatorHovered] = ImVec4(0.35f, 0.55f, 0.85f, 0.78f);
    colors[ImGuiCol_SeparatorActive]  = ImVec4(0.35f, 0.55f, 0.85f, 1.0f);

    colors[ImGuiCol_ResizeGrip]       = ImVec4(0.35f, 0.55f, 0.85f, 0.20f);
    colors[ImGuiCol_ResizeGripHovered]= ImVec4(0.35f, 0.55f, 0.85f, 0.67f);
    colors[ImGuiCol_ResizeGripActive] = ImVec4(0.35f, 0.55f, 0.85f, 0.95f);

    colors[ImGuiCol_CheckMark]        = ImVec4(0.35f, 0.55f, 0.85f, 1.0f);
    colors[ImGuiCol_SliderGrab]       = ImVec4(0.35f, 0.55f, 0.85f, 1.0f);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.45f, 0.65f, 0.95f, 1.0f);

    colors[ImGuiCol_Text]             = ImVec4(0.92f, 0.92f, 0.94f, 1.0f);
    colors[ImGuiCol_TextDisabled]     = ImVec4(0.50f, 0.50f, 0.52f, 1.0f);

    colors[ImGuiCol_DockingPreview]   = ImVec4(0.35f, 0.55f, 0.85f, 0.70f);
    colors[ImGuiCol_DockingEmptyBg]   = ImVec4(0.12f, 0.12f, 0.14f, 1.0f);
}

void ThemeManager::applyLight() {
    ImGuiStyle& style = ImGui::GetStyle();

    // Rounding
    style.WindowRounding = 4.0f;
    style.FrameRounding = 2.0f;
    style.GrabRounding = 2.0f;

    ImVec4* colors = style.Colors;

    colors[ImGuiCol_WindowBg]         = ImVec4(0.94f, 0.94f, 0.94f, 1.0f);
    colors[ImGuiCol_ChildBg]          = ImVec4(0.94f, 0.94f, 0.94f, 0.0f);
    colors[ImGuiCol_PopupBg]          = ImVec4(0.98f, 0.98f, 0.98f, 0.94f);
    colors[ImGuiCol_Border]           = ImVec4(0.70f, 0.70f, 0.72f, 0.50f);
    colors[ImGuiCol_BorderShadow]     = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

    colors[ImGuiCol_TitleBg]          = ImVec4(0.82f, 0.82f, 0.84f, 1.0f);
    colors[ImGuiCol_TitleBgActive]    = ImVec4(0.76f, 0.76f, 0.80f, 1.0f);
    colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.82f, 0.82f, 0.84f, 0.50f);

    colors[ImGuiCol_MenuBarBg]        = ImVec4(0.88f, 0.88f, 0.90f, 1.0f);

    colors[ImGuiCol_FrameBg]          = ImVec4(0.86f, 0.86f, 0.88f, 1.0f);
    colors[ImGuiCol_FrameBgHovered]   = ImVec4(0.78f, 0.78f, 0.82f, 1.0f);
    colors[ImGuiCol_FrameBgActive]    = ImVec4(0.72f, 0.72f, 0.78f, 1.0f);

    colors[ImGuiCol_Button]           = ImVec4(0.78f, 0.78f, 0.82f, 1.0f);
    colors[ImGuiCol_ButtonHovered]    = ImVec4(0.68f, 0.68f, 0.75f, 1.0f);
    colors[ImGuiCol_ButtonActive]     = ImVec4(0.60f, 0.60f, 0.68f, 1.0f);

    colors[ImGuiCol_Header]           = ImVec4(0.72f, 0.78f, 0.88f, 1.0f);
    colors[ImGuiCol_HeaderHovered]    = ImVec4(0.62f, 0.70f, 0.82f, 1.0f);
    colors[ImGuiCol_HeaderActive]     = ImVec4(0.55f, 0.65f, 0.78f, 1.0f);

    colors[ImGuiCol_Tab]              = ImVec4(0.82f, 0.82f, 0.86f, 1.0f);
    colors[ImGuiCol_TabHovered]       = ImVec4(0.62f, 0.70f, 0.82f, 1.0f);
    colors[ImGuiCol_TabSelected]      = ImVec4(0.72f, 0.78f, 0.88f, 1.0f);
    // Unfocused (dimmed) dock tabs - without these, docked panel tab labels
    // (Properties/History/Tools) kept the dark default and went unreadable
    // against the black light-theme text until hovered.
    colors[ImGuiCol_TabDimmed]         = ImVec4(0.84f, 0.84f, 0.88f, 1.0f);
    colors[ImGuiCol_TabDimmedSelected] = ImVec4(0.78f, 0.82f, 0.90f, 1.0f);

    colors[ImGuiCol_ScrollbarBg]      = ImVec4(0.90f, 0.90f, 0.92f, 0.50f);
    colors[ImGuiCol_ScrollbarGrab]    = ImVec4(0.68f, 0.68f, 0.72f, 1.0f);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.58f, 0.58f, 0.62f, 1.0f);
    colors[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.50f, 0.50f, 0.55f, 1.0f);

    colors[ImGuiCol_Separator]        = ImVec4(0.70f, 0.70f, 0.72f, 0.50f);
    colors[ImGuiCol_SeparatorHovered] = ImVec4(0.20f, 0.45f, 0.75f, 0.78f);
    colors[ImGuiCol_SeparatorActive]  = ImVec4(0.20f, 0.45f, 0.75f, 1.0f);

    colors[ImGuiCol_ResizeGrip]       = ImVec4(0.20f, 0.45f, 0.75f, 0.20f);
    colors[ImGuiCol_ResizeGripHovered]= ImVec4(0.20f, 0.45f, 0.75f, 0.67f);
    colors[ImGuiCol_ResizeGripActive] = ImVec4(0.20f, 0.45f, 0.75f, 0.95f);

    colors[ImGuiCol_CheckMark]        = ImVec4(0.20f, 0.45f, 0.75f, 1.0f);
    colors[ImGuiCol_SliderGrab]       = ImVec4(0.20f, 0.45f, 0.75f, 1.0f);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.15f, 0.35f, 0.65f, 1.0f);

    colors[ImGuiCol_Text]             = ImVec4(0.10f, 0.10f, 0.12f, 1.0f);
    colors[ImGuiCol_TextDisabled]     = ImVec4(0.45f, 0.45f, 0.48f, 1.0f);

    colors[ImGuiCol_DockingPreview]   = ImVec4(0.20f, 0.45f, 0.75f, 0.70f);
    colors[ImGuiCol_DockingEmptyBg]   = ImVec4(0.94f, 0.94f, 0.94f, 1.0f);
}

} // namespace materializr
