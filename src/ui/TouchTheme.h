#pragma once
// im-touch shell style pack (docs/im-touch-ui-plan.md, Phase 1).
//
// RAII scope that restyles ImGui for the modern/im-touch layouts - bigger
// touch targets, rounded chrome, the mockup's palette - pushed around the
// shell's windows each frame and popped before anything desktop-styled
// renders. Header-only; geometry scales with uiScale().
//
// Two palettes: the mockup's near-black dark (default) and a light variant
// driven by ThemeManager - Application calls setLightMode() each frame from
// the current Theme, so View → Theme and Settings → Appearance restyle the
// shells live, exactly like the classic layout.

#include "../ui_scale.h"
#include <imgui.h>

namespace materializr {
namespace touchui {

// Palette switch (set from ThemeManager's Theme each frame; cheap).
inline bool g_lightMode = false;
inline void setLightMode(bool light) { g_lightMode = light; }
inline bool lightMode() { return g_lightMode; }

// Corner-radius override, set from Application each frame next to
// setLightMode. -1 = every caller's soft rounded default (the modern
// layout's look); >= 0 = a flat radius in unscaled px applied across the
// chrome AND the widget kit (im-touch asks for crisp 2 px corners, Steve).
inline float g_cornerOverride = -1.0f;
inline void setCornerRadius(float px) { g_cornerOverride = px; }
// Resolve a rounding: `softScaled` is the caller's ALREADY-uiScaled soft
// default, returned untouched unless the override is active.
inline float radius(float softScaled) {
    return g_cornerOverride >= 0.0f ? g_cornerOverride * uiScale() : softScaled;
}

// Palette. Exposed for the widget kit's custom draws. Each colour has a
// dark (mockup) and light counterpart, matched for the same role/contrast.
inline ImVec4 chromeBg()     { return g_lightMode ? ImVec4(0.937f, 0.945f, 0.957f, 1.0f)   // #EFF1F4
                                                  : ImVec4(0.043f, 0.051f, 0.067f, 1.0f); } // #0B0D11
inline ImVec4 panelBg()      { return g_lightMode ? ImVec4(0.973f, 0.978f, 0.984f, 1.0f)   // #F8F9FB
                                                  : ImVec4(0.063f, 0.075f, 0.094f, 1.0f); } // #101318
inline ImVec4 rowBg()        { return g_lightMode ? ImVec4(0.886f, 0.902f, 0.925f, 1.0f)   // #E2E6EC
                                                  : ImVec4(0.102f, 0.118f, 0.145f, 1.0f); } // #1A1E25
inline ImVec4 hairline()     { return g_lightMode ? ImVec4(0.812f, 0.831f, 0.859f, 1.0f)   // #CFD4DB
                                                  : ImVec4(0.133f, 0.145f, 0.169f, 1.0f); } // #22252B
inline ImVec4 accentFill()   { return g_lightMode ? ImVec4(0.475f, 0.635f, 0.910f, 1.0f)   // #79A2E8
                                                  : ImVec4(0.561f, 0.706f, 0.949f, 1.0f); } // #8FB4F2
inline ImVec4 accentDeep()   { return ImVec4(0.239f, 0.435f, 0.851f, 1.0f); }               // #3D6FD9
inline ImVec4 textPrimary()  { return g_lightMode ? ImVec4(0.110f, 0.125f, 0.153f, 1.0f)   // #1C2027
                                                  : ImVec4(0.933f, 0.941f, 0.953f, 1.0f); } // #EEF0F3
inline ImVec4 textDim()      { return g_lightMode ? ImVec4(0.400f, 0.424f, 0.459f, 1.0f)   // #666C75
                                                  : ImVec4(0.541f, 0.561f, 0.596f, 1.0f); } // #8A8F98
inline ImVec4 onAccent()     { return g_lightMode ? ImVec4(1.0f, 1.0f, 1.0f, 1.0f)
                                                  : ImVec4(0.051f, 0.075f, 0.125f, 1.0f); } // text on accent
// Interactive-state fills (rowBg's hover/press neighbours) - shared by
// pushChrome and the widget kit's custom draws so the two can't drift.
inline ImVec4 hoverBg()      { return g_lightMode ? ImVec4(0.855f, 0.875f, 0.902f, 1.0f)
                                                  : ImVec4(0.16f, 0.19f, 0.24f, 1.0f); }
inline ImVec4 pressBg()      { return g_lightMode ? ImVec4(0.808f, 0.835f, 0.871f, 1.0f)
                                                  : ImVec4(0.20f, 0.24f, 0.31f, 1.0f); }
// Subtle full-row hover (listRow) - between panelBg and rowBg.
inline ImVec4 rowHoverBg()   { return g_lightMode ? ImVec4(0.918f, 0.929f, 0.945f, 1.0f)
                                                  : ImVec4(0.09f, 0.10f, 0.13f, 1.0f); }

// Chrome-only subset: colors + rounding + window padding, but NOT the
// content metrics (FramePadding / ItemSpacing). This is what wraps the WHOLE
// frame while modern/im-touch is on, so dialogs / popups / the context menu
// pick up the rounded padded look - without inflating the metrics that
// classic dialog code sized its fixed-width buttons and windows against
// (which clipped their labels off the right edge).
inline void pushChrome() {
    const float s = uiScale();
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,   radius(10.0f * s));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding,   radius(12.0f * s));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding,   radius(10.0f * s));
    // The shell's edge-flush bars opt back out with a local WindowRounding=0.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,  radius(12.0f * s));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,   ImVec2(14.0f * s, 12.0f * s));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize,   14.0f * s);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize,     24.0f * s);

    ImGui::PushStyleColor(ImGuiCol_TitleBg,          chromeBg());
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive,    panelBg());
    ImGui::PushStyleColor(ImGuiCol_TitleBgCollapsed, chromeBg());
    ImGui::PushStyleColor(ImGuiCol_WindowBg,       chromeBg());
    ImGui::PushStyleColor(ImGuiCol_PopupBg,        panelBg());
    ImGui::PushStyleColor(ImGuiCol_Border,         hairline());
    ImGui::PushStyleColor(ImGuiCol_Separator,      hairline());
    ImGui::PushStyleColor(ImGuiCol_FrameBg,        rowBg());
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, hoverBg());
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive,  pressBg());
    ImGui::PushStyleColor(ImGuiCol_Button,         rowBg());
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,  hoverBg());
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,   pressBg());
    ImGui::PushStyleColor(ImGuiCol_Text,           textPrimary());
    ImGui::PushStyleColor(ImGuiCol_TextDisabled,   textDim());
    ImGui::PushStyleColor(ImGuiCol_CheckMark,      accentFill());
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered,  hoverBg());
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,   pressBg());
    ImGui::PushStyleColor(ImGuiCol_Header,         rowBg());
}

inline void popChrome() {
    ImGui::PopStyleColor(19);
    ImGui::PopStyleVar(7);
}

// Full shell style: the chrome plus the touch-comfy content metrics. Only
// the shell (and its own popups) render under this - classic dialog code
// sized against classic metrics must NOT (see pushChrome).
inline void push() {
    const float s = uiScale();
    pushChrome();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14.0f * s, 9.0f * s));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,  ImVec2(10.0f * s, 10.0f * s));
}

inline void pop() {
    ImGui::PopStyleVar(2);
    popChrome();
}

struct Scope {
    Scope()  { push(); }
    ~Scope() { pop(); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};

} // namespace touchui
} // namespace materializr
