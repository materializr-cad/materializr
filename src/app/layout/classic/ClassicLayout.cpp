// Classic layout (UiLayout::Classic) - the traditional desktop shell: main
// menu bar, docked panels (Tools / Items / History / Properties via the
// shared dockspace), and the status bar. The menu ITEM lists themselves are
// shared with the other layouts (layout/LayoutCommon.cpp); only the menu BAR
// hosting them is classic-specific. The docked panel windows are submitted
// from Application::run(), gated on classicLayout().

#include "app/Application.h"
#include "touch_mode.h"
#include "ui_scale.h"

#include <imgui.h>
#include "../../../i18n.h"

namespace materializr {

void Application::renderMenuBar() {
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu(materializr::tr("File"))) { renderFileMenuItems(); ImGui::EndMenu(); }
        if (ImGui::BeginMenu(materializr::tr("Edit"))) { renderEditMenuItems(); ImGui::EndMenu(); }
        if (ImGui::BeginMenu(materializr::tr("View"))) { renderViewMenuItems(); ImGui::EndMenu(); }
        if (ImGui::BeginMenu(materializr::tr("Help"))) { renderHelpMenuItems(); ImGui::EndMenu(); }
        // (No soft-keyboard toggle: the numeric fields now raise the native
        // keyboard on focus, so the manual force-toggle is redundant here.)
        ImGui::EndMainMenuBar();
    }
}

void Application::renderPanelCollapseHandles() {
    // Touch-only edge tabs that collapse/restore each docked side column. They
    // anchor to the panel/viewport boundary, which slides to the screen edge
    // once a side is collapsed - so a hidden panel still has a visible pull-tab.
    // Desktop uses View > Hide Panels / F9 instead, so this is touch-gated.
    if (!materializr::touchMode()) return;
    if (m_viewportWinW <= 0.0f || m_viewportWinH <= 0.0f) return;

    const float hw = uiW(22.0f);                          // tab width
    const float hh = uiW(80.0f);                          // tab height (touch target)
    const float cy = m_viewportWinY + m_viewportWinH * 0.5f - hh * 0.5f;
    const ImGuiViewport* vp = ImGui::GetMainViewport();

    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBackground;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));

    // Left tab - flush against the viewport's left edge, sitting just INSIDE the
    // viewport (not over the Tools panel, whose space is tight). When the panel
    // is collapsed the viewport edge is the screen edge, so the tab hugs it.
    // The arrow points the way the tap moves the panel: '<' collapses it toward
    // the edge, '>' pulls it back out.
    {
        float x = m_viewportWinX;
        if (x < vp->WorkPos.x) x = vp->WorkPos.x;
        ImGui::SetNextWindowPos(ImVec2(x, cy));
        ImGui::SetNextWindowSize(ImVec2(hw, hh));
        ImGui::Begin("##collapseLeft", nullptr, flags);
        if (ImGui::Button(m_leftPanelHidden ? ">" : "<", ImVec2(hw, hh))) {
            m_leftPanelHidden = !m_leftPanelHidden;
            saveAppSettings();
        }
        ImGui::End();
    }
    // Right tab - flush against the viewport's right edge, sitting just INSIDE
    // the viewport (not over the Items/Properties column).
    {
        float x = m_viewportWinX + m_viewportWinW - hw;
        const float maxX = vp->WorkPos.x + vp->WorkSize.x - hw;
        if (x > maxX) x = maxX;
        ImGui::SetNextWindowPos(ImVec2(x, cy));
        ImGui::SetNextWindowSize(ImVec2(hw, hh));
        ImGui::Begin("##collapseRight", nullptr, flags);
        if (ImGui::Button(m_rightPanelHidden ? "<" : ">", ImVec2(hw, hh))) {
            m_rightPanelHidden = !m_rightPanelHidden;
            saveAppSettings();
        }
        ImGui::End();
    }

    ImGui::PopStyleVar();
}

} // namespace materializr
