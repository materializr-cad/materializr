// Layout-shared chrome: everything every interface layout (classic / modern /
// im-touch) renders from the SAME code so the layouts can't drift apart in
// fundamentals - the menu item lists (incl. plugin menu contributions), the
// dockspace host, the overflow popup, and the shared undo helpers. See
// LayoutCommon.h for the keep-in-lockstep contract.

#include "app/Application.h"
#include "app/Window.h"
#include "ui/MeasureTool.h"
#include "app/layout/LayoutCommon.h"
#include "core/Document.h"
#include "core/History.h"
#include "core/SelectionManager.h"
#include "modeling/Sketch.h"
#include "modeling/SketchTool.h"
#include "modeling/SketchTransformOp.h"
#include "plugin/PluginContext.h"
#include "plugin/PluginRegistry.h"
#include "io/FileDialogs.h"   // Import → From Project… picker
#include "app/ProjectSession.h" // Tabs submenu reads session names
#include <filesystem>
#include "ui/AboutDialog.h"
#include "ui/HelpPanel.h"
#include "ui/LandingPage.h"   // File → New Project dismisses the landing page
#include "ui/LogoTexture.h"
#include "ui/ShortcutsPanel.h"
#include "ui/ThemeManager.h"
#include "ui/Toolbar.h"
#include "ui/TouchIcons.h"
#include "ui/UiTheme.h"   // accentText for the touch tabs sheet heading
#include "viewport/Viewport.h"
#include "gl_common.h"
#include "touch_mode.h"

#include <imgui.h>
#include <imgui_internal.h> // dock-node tab-bar policy (per-node LocalFlags)

#include <BRepAdaptor_Curve.hxx>
#include <BRepBndLib.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <Geom_CylindricalSurface.hxx>
#include <Geom_Plane.hxx>
#include <Geom_Surface.hxx>
#include <TopoDS.hxx>

#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>   // std::max - tab-row width reservation
#include "../../i18n.h"
#include "../../i18n.h"
#include "../../i18n.h"
#include "../../i18n.h"
#include "../../i18n.h"

namespace materializr {

namespace layoutui {

ImTextureID logoTexture() {
    static GLuint tex = 0;
    if (!tex) {
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        // Pin the unpack state before the upload. This texture is created
        // lazily on the first frame it's drawn, inheriting whatever GL pixel-
        // store state the prior frame left set - if some earlier texture upload
        // left GL_UNPACK_ROW_LENGTH non-zero (or a non-4 alignment), the logo
        // reads its rows at the wrong stride and comes out garbled, and since
        // the texture is static that corruption sticks for the whole session.
        GLint prevRowLen = 0, prevAlign = 4;
        glGetIntegerv(GL_UNPACK_ROW_LENGTH, &prevRowLen);
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &prevAlign);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, materializr::kLogoTexW,
                     materializr::kLogoTexH, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     materializr::kLogoTexRGBA);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, prevRowLen);
        glPixelStorei(GL_UNPACK_ALIGNMENT, prevAlign);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    return (ImTextureID)(intptr_t)tex;
}

} // namespace layoutui

void Application::renderDockspace() {
    // Host the dockspace in a window inset above the status bar so docked panels
    // (e.g. the Tools window's bottom Delete button) aren't covered by the
    // full-width status bar overlay. Reuse the original DockSpaceOverViewport
    // dockspace id (0x08BD597D) so the saved imgui.ini layout still binds.
    const float statusBarHeight = 24.0f;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, vp->WorkSize.y - statusBarHeight));
    ImGui::SetNextWindowViewport(vp->ID);

    ImGuiWindowFlags hostFlags =
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoNavFocus;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    // Transparent host + a pass-through central node so the OpenGL scene shows
    // through. This matters now that the host is submitted EVERY frame (incl.
    // modern/im-touch): there the viewport is undocked, leaving the central
    // node empty - an opaque host/node would paint dark over the 3D view.
    // Docked classic windows cover the host anyway, so it looks identical there.
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::Begin("DockHost", nullptr, hostFlags);
    ImGui::PopStyleVar(3);
    ImGui::DockSpace(0x08BD597Du, ImVec2(0.0f, 0.0f),
                     ImGuiDockNodeFlags_PassthruCentralNode);

    // Per-node tab-bar policy. The viewport's tab bar is permanently OFF
    // (NoTabBar = no tab AND no re-show triangle) - it's the whole app, never
    // something to label or hide. Every panel ALWAYS shows its tab (a label +
    // drag handle) and loses the "Hide tab bar" menu, so panel visibility is
    // owned solely by Settings > Panels. Applied to LocalFlags each frame so it
    // overrides whatever the saved imgui.ini had (e.g. the central node and the
    // Interactions node both shipped with HiddenTabBar=1).
    auto setNodeFlags = [](const char* win, ImGuiDockNodeFlags set,
                           ImGuiDockNodeFlags clear) {
        if (ImGuiWindow* w = ImGui::FindWindowByName(win))
            if (w->DockNode) {
                w->DockNode->LocalFlags |= set;
                w->DockNode->LocalFlags &= ~clear;
            }
    };
    setNodeFlags("Viewport", ImGuiDockNodeFlags_NoTabBar, 0);
    const ImGuiDockNodeFlags kPanelSet   = ImGuiDockNodeFlags_NoWindowMenuButton;
    const ImGuiDockNodeFlags kPanelClear = ImGuiDockNodeFlags_HiddenTabBar |
                                           ImGuiDockNodeFlags_AutoHideTabBar;
    setNodeFlags("Tools",        kPanelSet, kPanelClear);
    setNodeFlags("Interactions", kPanelSet, kPanelClear);
    setNodeFlags("Items",        kPanelSet, kPanelClear);
    setNodeFlags("History",      kPanelSet, kPanelClear);
    setNodeFlags("Properties",   kPanelSet, kPanelClear);
    ImGui::End();
}

bool Application::touchCanUndo() const {
    if (m_inSketchMode) {
        // An in-progress shape can always be cancelled; otherwise only committed
        // sketch edits (steps after the sketch's entry) are undoable here.
        if (m_sketchTool && m_sketchTool->isPlacing()) return true;
        return m_history->canUndo() &&
               m_history->currentStep() > m_sketchEntryHistoryStep;
    }
    return m_history->canUndo();
}

void Application::touchUndo() {
    if (m_inSketchMode) {
        // Mid-placement Undo backs out the in-progress shape first - the editor
        // convention, and what the sketch's own Ctrl+Z does.
        if (m_sketchTool && m_sketchTool->isPlacing()) {
            m_sketchTool->onCancel();
            return;   // sketch geometry only; no body mesh depends on it
        }
        // Undo committed sketch edits, but never past the sketch's entry into
        // history: rolling the host body back under a live sketch crashes.
        if (m_history->canUndo() &&
            m_history->currentStep() > m_sketchEntryHistoryStep) {
            undoWithCascade();                 // undoes + re-cascades the body
            if (m_activeSketch) m_activeSketch->pruneOrphanPoints();
        }
        return;
    }
    if (m_history->canUndo()) undoWithCascade();
}

// ─── Tab UI helpers (shared by all three layouts' tab affordances) ──────────

bool Application::bodyExists(int bodyId) const {
    if (bodyId < 0 || !m_document) return false;
    const auto ids = m_document->getAllBodyIds();
    return std::find(ids.begin(), ids.end(), bodyId) != ids.end();
}

std::string Application::sessionDisplayLabel(size_t i) const {
    if (i >= m_sessions.size()) return "Untitled";
    // Same fallback chain as projectDisplayName(): explicit name → file
    // basename → Untitled. The active tab reads the LIVE working copies;
    // inactive tabs read their stashed ones.
    const std::string& name = (i == m_activeSession) ? m_currentProjectName
                                                     : m_sessions[i]->projectName;
    const std::string& path = (i == m_activeSession) ? m_currentProjectPath
                                                     : m_sessions[i]->projectPath;
    if (!name.empty()) return name;
    if (!path.empty()) return std::filesystem::path(path).filename().string();
    return "Untitled";
}

bool Application::sessionDirty(size_t i) const {
    if (i >= m_sessions.size()) return false;
    if (i == m_activeSession) return isDirty();
    const ProjectSession& s = *m_sessions[i];
    return (s.history && s.history->currentStep() != s.savedAtHistoryStep) ||
           s.unsavedNonHistoryChanges;
}

bool Application::activateTabFor(size_t i) {
    return i == m_activeSession || switchToSession(i);
}

void Application::renderTabMenuItems(size_t i) {
    // Actions on a non-active tab activate it first; a refused switch
    // (mid-sketch etc.) already toasted, so the action just doesn't happen.
    if (ImGui::MenuItem(materializr::tr("Save"))) {
        if (activateTabFor(i)) saveProjectQuick();
    }
    if (ImGui::MenuItem(materializr::tr("Save As..."))) {
        if (activateTabFor(i)) saveProject();
    }
    ImGui::Separator();
    if (ImGui::MenuItem(materializr::tr("Close Tab"))) {
        if (activateTabFor(i))
            guardedOpen([this]() { closeSession(m_activeSession); });
    }
}

bool Application::openNewTab() {
    const size_t idx = createSession();
    if (switchToSession(idx)) return true;
    closeSession(idx);   // refused switch: drop the orphan background tab
    return false;
}

void Application::renderNewTabMenuBody() {
    if (ImGui::MenuItem(materializr::tr("New Project"))) openNewTab();
    // The open flavors land IN the new tab. Cancelling the picker leaves an
    // empty tab behind (browser-style about:blank) - one click to close.
    if (ImGui::MenuItem(materializr::tr("Open Project..."))) {
        if (openNewTab()) loadProject();
    }
    if (ImGui::BeginMenu(materializr::tr("Open Recent"), !m_recentProjects.empty())) {
        // Snapshot: openRecentProject() mutates m_recentProjects.
        std::vector<AppSettings::RecentProject> snapshot = m_recentProjects;
        for (size_t i = 0; i < snapshot.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::MenuItem(snapshot[i].name.c_str())) {
                if (openNewTab()) openRecentProject(snapshot[i]);
            }
            if (ImGui::IsItemHovered() && !snapshot[i].ref.empty())
                ImGui::SetTooltip("%s", snapshot[i].ref.c_str());
            ImGui::PopID();
        }
        ImGui::EndMenu();
    }
}

void Application::renderViewportTabBar() {
    // Classic only: the strip lives INSIDE the Viewport window, above the 3D
    // image, styled like the dock tab bars - but it is a plain ImGui tab bar,
    // not a dock node, so tabs cannot be dragged into the Tools/Items docks.
    const ImGuiTabBarFlags barFlags = ImGuiTabBarFlags_FittingPolicyScroll;
    if (!ImGui::BeginTabBar("##projectTabs", barFlags)) return;
    // On a sync frame (the active session changed OUTSIDE this bar - menus,
    // Ctrl+Tab, a refused switch), ImGui's internal selection still points at
    // the OLD tab for this frame. Interpreting that stale "visible" as a user
    // click would silently switch right back - so clicks are ignored for the
    // whole sync frame while SetSelected drags ImGui to the real active tab.
    const bool syncing = m_tabSelectionSync;
    m_tabSelectionSync = false;
    // Report hover so a press-and-hold here becomes the right-click that
    // BeginPopupContextItem below is waiting for (see m_tabBarHovered).
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows |
                               ImGuiHoveredFlags_AllowWhenBlockedByPopup))
        m_tabBarHovered = true;
    bool closedOne = false;
    for (size_t i = 0; i < m_sessions.size() && !closedOne; ++i) {
        ImGui::PushID(static_cast<int>(i));
        ImGuiTabItemFlags tif = ImGuiTabItemFlags_NoReorder;
        if (i == m_activeSession && syncing)
            tif |= ImGuiTabItemFlags_SetSelected;
        if (sessionDirty(i)) tif |= ImGuiTabItemFlags_UnsavedDocument;
        bool open = true;
        const bool visible =
            ImGui::BeginTabItem(sessionDisplayLabel(i).c_str(), &open, tif);
        if (ImGui::BeginPopupContextItem("tabctx")) {
            renderTabMenuItems(i);
            ImGui::EndPopup();
        }
        if (visible) {
            ImGui::EndTabItem();
            // Outside sync frames, a visible non-active tab = a user click.
            // A refused switch re-arms the sync so the visual snaps back.
            if (!syncing && i != m_activeSession) {
                if (!switchToSession(i)) m_tabSelectionSync = true;
            }
        }
        if (!open) {
            // The tab's × - same guarded flow as the menu item.
            if (activateTabFor(i))
                guardedOpen([this]() { closeSession(m_activeSession); });
            closedOne = true;   // indices may have shifted; finish this frame
        }
        ImGui::PopID();
    }
    if (!closedOne &&
        ImGui::TabItemButton("+", ImGuiTabItemFlags_Trailing |
                                      ImGuiTabItemFlags_NoTooltip))
        ImGui::OpenPopup("##newTabMenu");
    if (ImGui::BeginPopup("##newTabMenu")) {
        renderNewTabMenuBody();
        ImGui::EndPopup();
    }
    ImGui::EndTabBar();
}

void Application::renderTouchTabsSheet() {
    // Im-touch: opened by tapping the project-name chip. Rows switch tabs;
    // each row's ⋮ opens the shared Save / Save As / Close menu; the last
    // row starts a fresh tab.
    if (!ImGui::BeginPopup("##TouchTabs")) return;
    ImGui::TextColored(materializr::accentText(), "%s", materializr::tr("Open projects"));
    ImGui::Separator();
    for (size_t i = 0; i < m_sessions.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        std::string label = sessionDisplayLabel(i);
        if (sessionDirty(i)) label += " \xe2\x80\xa2";
        // The row and its ... are SEPARATE hit areas. Previously the row was a
        // full-width MenuItem with the ... drawn on top of it, so a tap on the
        // ... hit the MenuItem underneath: it switched tabs and - because a
        // MenuItem closes its popup on activation - took the sheet down with
        // it, so the menu could never appear. Reserve the width, and use a
        // Selectable (which does NOT auto-close) so the two can coexist.
        const ImGuiStyle& st = ImGui::GetStyle();
        const float moreW = ImGui::CalcTextSize(MZ_ICON_MORE).x +
                            st.FramePadding.x * 2.0f;
        const float rowW  = std::max(1.0f, ImGui::GetContentRegionAvail().x -
                                               moreW - st.ItemSpacing.x);
        if (ImGui::Selectable(label.c_str(), i == m_activeSession,
                              ImGuiSelectableFlags_None, ImVec2(rowW, 0.0f))) {
            switchToSession(i);
            ImGui::CloseCurrentPopup();   // MenuItem did this implicitly
        }
        ImGui::SameLine(0.0f, st.ItemSpacing.x);
        if (ImGui::Button(MZ_ICON_MORE, ImVec2(moreW, 0.0f)))
            ImGui::OpenPopup("touchtabctx");
        if (ImGui::BeginPopup("touchtabctx")) {
            renderTabMenuItems(i);
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    ImGui::Separator();
    // Same trio the desktop "+" offers - opens land in the new tab.
    renderNewTabMenuBody();
    ImGui::EndPopup();
}

// The four menu bodies, shared by classic's menu bar and the modern/im-touch
// overflow popup - one item list each, so the layouts cannot drift.
void Application::renderFileMenuItems(bool withSettings) {
    if (ImGui::MenuItem(materializr::tr("Home Screen"))) goToHomeScreen();
    if (ImGui::MenuItem(materializr::tr("Open Project..."), "Ctrl+O")) loadProject();
    // Open Recent - persisted, most-recent-first. Greyed when empty.
    if (ImGui::BeginMenu(materializr::tr("Open Recent"), !m_recentProjects.empty())) {
        // Snapshot: openRecentProject() mutates m_recentProjects.
        std::vector<AppSettings::RecentProject> snapshot = m_recentProjects;
        for (size_t i = 0; i < snapshot.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::MenuItem(snapshot[i].name.c_str()))
                openRecentProject(snapshot[i]);
            if (ImGui::IsItemHovered() && !snapshot[i].ref.empty())
                ImGui::SetTooltip("%s", snapshot[i].ref.c_str());
            ImGui::PopID();
        }
        ImGui::Separator();
        if (ImGui::MenuItem(materializr::tr("Clear Recent"))) {
            m_recentProjects.clear();
            saveAppSettings();
        }
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem(materializr::tr("Save Project"), "Ctrl+S")) saveProjectQuick();
    if (ImGui::MenuItem(materializr::tr("Save Project As..."))) saveProject();
    // A new project opens in its own tab (non-destructive - the current
    // project keeps its tab); the landing page's New Project tile still
    // resets in place, where the leaving-home guard has already run.
    if (ImGui::MenuItem(materializr::tr("New Project"))) {
        if (m_landingPage) m_landingPage->setVisible(false);
        openNewTab();
    }
    ImGui::Separator();
    if (ImGui::BeginMenu(materializr::tr("Tabs"), m_sessions.size() > 1)) {
        for (size_t i = 0; i < m_sessions.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::MenuItem(sessionDisplayLabel(i).c_str(),
                                i == m_activeSession ? "(current)" : nullptr,
                                i == m_activeSession))
                switchToSession(i);
            ImGui::PopID();
        }
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem(materializr::tr("Close Tab"))) {
        // Same prompt-then-act path as every destructive project action.
        guardedOpen([this]() { closeSession(m_activeSession); });
    }
    ImGui::Separator();

    // Build Import submenu from IOFormat contributions
    auto& formats = PluginRegistry::instance().ioFormats();
    bool hasImporters = false;
    for (auto& fmt : formats) { if (fmt.canImport) { hasImporters = true; break; } }
    if (hasImporters && ImGui::BeginMenu(materializr::tr("Import"))) {
        for (size_t i = 0; i < formats.size(); ++i) {
            auto& fmt = formats[i];
            if (!fmt.canImport || !fmt.importFn) continue;
            ImGui::PushID(static_cast<int>(i));
            std::string label = fmt.name + "...";
            if (ImGui::MenuItem(label.c_str())) {
                fmt.importFn(*m_pluginContext, "");
            }
            ImGui::PopID();
        }
        ImGui::Separator();
        // Cross-project parts: pick another project file, then choose which
        // of its bodies/sketches to copy in (baked, non-parametric).
        if (ImGui::MenuItem(materializr::tr("From Project..."))) {
            FileDialogs::openFile("Import from Project",
                {{"Materializr Projects", "*.mzr *.materializr"}},
                [this](const std::string& p) {
                    if (p.empty()) return;
                    openPartsPicker(p,
                        std::filesystem::path(p).filename().string(),
                        /*intoNewProject=*/false);
                });
        }
        ImGui::EndMenu();
    }

    // Build Export submenu from IOFormat contributions
    bool hasExporters = false;
    for (auto& fmt : formats) { if (fmt.canExport) { hasExporters = true; break; } }
    if (hasExporters && ImGui::BeginMenu(materializr::tr("Export"))) {
        for (size_t i = 0; i < formats.size(); ++i) {
            auto& fmt = formats[i];
            if (!fmt.canExport || !fmt.exportFn) continue;
            ImGui::PushID(static_cast<int>(i) + 1000);
            std::string label = fmt.name + "...";
            if (ImGui::MenuItem(label.c_str())) {
                fmt.exportFn(*m_pluginContext, "");
            }
            ImGui::PopID();
        }
        ImGui::EndMenu();
    }

    if (withSettings) {
        ImGui::Separator();
        if (ImGui::MenuItem(materializr::tr("Settings..."))) {
            // Stage the current bindings so the dialog can Cancel cleanly.
            m_settingsOrbitButton = m_orbitButton;
            m_settingsPanButton = m_panButton;
            m_showSettings = true;
            m_settingsRaise = true;
        }
    }
    ImGui::Separator();
    if (ImGui::MenuItem(materializr::tr("Exit"), "Alt+F4")) m_window->requestClose(true);
}

void Application::renderEditMenuItems() {
    // Disabled while a legacy preview is live: those previews
    // undo/re-push their op per frame, and an outside undo pops the
    // preview op so the preview's NEXT cycle pops the user's last
    // COMMITTED op instead - which then gets erased for good when
    // the preview pushes over the redo tail. (How "pull, confirm,
    // pull the other way" ate the first body.)
    const bool histLocked = anyInteractivePreviewActive();
    if (ImGui::MenuItem(materializr::tr("Undo"), "Ctrl+Z", false,
                        !histLocked && m_history->canUndo())) {
        undoWithCascade();
    }
    if (ImGui::MenuItem(materializr::tr("Redo"), "Ctrl+Y", false,
                        !histLocked && m_history->canRedo())) {
        redoWithCascade();
    }
}

void Application::renderConstructionMenuItems() {
    // Detect which plane/axis derivations the current selection supports -
    // mirrors Toolbar::renderAddPlaneMenu / renderAddAxisMenu (keep in sync).
    int planarFaces = 0, planeCount = 0, vertexCount = 0;
    bool haveCyl = false, straightEdge = false, haveAxis = false;
    if (m_selection) {
        for (const auto& e : m_selection->getSelection()) {
            if (e.type == SelectionType::Plane)  { ++planeCount;  continue; }
            if (e.type == SelectionType::Axis)   { haveAxis = true; continue; }
            if (e.type == SelectionType::Vertex) { ++vertexCount; continue; }
            if (e.shape.IsNull()) continue;
            try {
                if (e.type == SelectionType::Face) {
                    Handle(Geom_Surface) srf = BRep_Tool::Surface(TopoDS::Face(e.shape));
                    if (!srf.IsNull()) {
                        if (srf->IsKind(STANDARD_TYPE(Geom_Plane))) ++planarFaces;
                        else if (!Handle(Geom_CylindricalSurface)::DownCast(srf).IsNull())
                            haveCyl = true;
                    }
                } else if (e.type == SelectionType::Edge) {
                    BRepAdaptor_Curve ad(TopoDS::Edge(e.shape));
                    if (ad.GetType() == GeomAbs_Line) straightEdge = true;
                }
            } catch (...) {}
        }
    }
    const bool midplane   = (planarFaces >= 2) || (planeCount >= 2);
    const bool twoVerts   = (vertexCount >= 2);
    const bool faceNormal = (planarFaces >= 1);
    const bool anyPlane = m_pluginContext &&
                          (midplane || haveCyl || haveAxis || straightEdge);
    const bool anyAxis  = m_pluginContext &&
                          (haveCyl || straightEdge || twoVerts || faceNormal || midplane);

    // Plane ▸ and Axis ▸ are always present so the catalogue is discoverable.
    // Each leads with the BASE "New …" creator (the world-plane/-axis popup -
    // always available, selection or not), then the modes derived FROM the
    // selection; with nothing suitable selected the derived section explains
    // what to pick instead of vanishing.
    if (ImGui::BeginMenu(materializr::tr("Plane"))) {
        if (m_pluginContext && ImGui::MenuItem(materializr::tr("New Plane...")))
            m_pluginContext->requestInteractiveOp(InteractiveOp::ConstructionPlane);
        ImGui::Separator();
        if (!anyPlane) {
            ImGui::MenuItem(materializr::tr("Select what to derive from:"), nullptr, false, false);
            ImGui::MenuItem(materializr::tr("2 flat faces/planes  - midplane"), nullptr, false, false);
            ImGui::MenuItem(materializr::tr("a cylinder  - tangent / normal"), nullptr, false, false);
            ImGui::MenuItem(materializr::tr("an edge or axis  - normal plane"), nullptr, false, false);
        } else {
            if (midplane && ImGui::MenuItem(materializr::tr("Midplane (between the 2 selected)")))
                m_pluginContext->requestInteractiveOp(InteractiveOp::Midplane);
            if (haveCyl) {
                if (ImGui::MenuItem(materializr::tr("Tangent to cylinder")))
                    m_pluginContext->requestInteractiveOp(InteractiveOp::TangentPlane);
                if (ImGui::MenuItem(materializr::tr("Perpendicular to cylinder axis")))
                    m_pluginContext->requestInteractiveOp(InteractiveOp::PlaneNormalToAxis);
                if (ImGui::MenuItem(materializr::tr("Through cylinder axis (longitudinal)")))
                    m_pluginContext->requestInteractiveOp(InteractiveOp::PlaneThroughAxis);
            } else if (haveAxis || straightEdge) {
                if (ImGui::MenuItem(straightEdge ? "Normal to edge" : "Normal to axis"))
                    m_pluginContext->requestInteractiveOp(InteractiveOp::PlaneNormalToAxis);
            }
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(materializr::tr("Axis"))) {
        if (m_pluginContext && ImGui::MenuItem(materializr::tr("New Axis...")))
            m_pluginContext->requestInteractiveOp(InteractiveOp::ConstructionAxis);
        ImGui::Separator();
        if (!anyAxis) {
            ImGui::MenuItem(materializr::tr("Select what to derive from:"), nullptr, false, false);
            ImGui::MenuItem(materializr::tr("a cylinder or straight edge"), nullptr, false, false);
            ImGui::MenuItem(materializr::tr("2 vertices / a flat face / 2 planes"), nullptr, false, false);
        } else {
            if (haveCyl && ImGui::MenuItem(materializr::tr("From cylinder axis")))
                m_pluginContext->requestInteractiveOp(InteractiveOp::AxisFromCylinder);
            if (straightEdge && ImGui::MenuItem(materializr::tr("Along edge")))
                m_pluginContext->requestInteractiveOp(InteractiveOp::AxisAlongEdge);
            if (twoVerts && ImGui::MenuItem(materializr::tr("Through two vertices")))
                m_pluginContext->requestInteractiveOp(InteractiveOp::AxisTwoPoints);
            if (faceNormal && ImGui::MenuItem(materializr::tr("Normal to face")))
                m_pluginContext->requestInteractiveOp(InteractiveOp::AxisNormalToFace);
            if (midplane && ImGui::MenuItem(materializr::tr("Intersection of two planes")))
                m_pluginContext->requestInteractiveOp(InteractiveOp::AxisTwoPlanes);
        }
        ImGui::EndMenu();
    }
}

void Application::renderViewMenuItems() {
    if (ImGui::MenuItem(materializr::tr("Reset Camera"), "Home")) m_viewport->getCamera().reset();
    // The F shortcut's menu twin - and the only way to frame on touch.
    if (ImGui::MenuItem(materializr::tr("Frame Selection"), "F")) frameSelection();
    // Measure lives here now - one home for it across layouts instead of a
    // toolbar/rail button duplicated per context. Drops the user at the
    // measure mode picker (Object / Edge / Point-to-Point).
    if (ImGui::MenuItem(materializr::tr("Measure..."))) {
        if (m_measureTool) m_measureTool->setMode(MeasureMode::PickMode);
    }
    if (ImGui::MenuItem(materializr::tr("Section View"), nullptr, &m_sectionEnabled)) {
        m_sectionDirty = true;
        if (m_sectionEnabled) {
            // Aim the plane through the middle of the visible
            // bodies so enabling it visibly halves the scene -
            // a zero-offset plane at the world origin can sit
            // entirely outside (or under) everything.
            try {
                Bnd_Box bb;
                for (int id : m_document->getAllBodyIds())
                    if (m_document->isBodyVisible(id))
                        BRepBndLib::Add(m_document->getBody(id), bb);
                if (!bb.IsVoid()) {
                    double x0, y0, z0, x1, y1, z1;
                    bb.Get(x0, y0, z0, x1, y1, z1);
                    gp_Pnt c(0.5 * (x0 + x1), 0.5 * (y0 + y1),
                             0.5 * (z0 + z1));
                    gp_Pln pl = sectionBasePlane();
                    m_sectionOffset = static_cast<float>(
                        gp_Vec(pl.Location(), c)
                            .Dot(gp_Vec(pl.Axis().Direction())));
                }
            } catch (...) {}
        }
    }
    ImGui::Separator();
    // Collapse the docked side panels to give the 3D view the whole
    // window - a fallback for small screens (and a quick "maximize
    // canvas" anywhere). The panels keep their docked widths and snap
    // back on toggle. F9 on a keyboard; touch gets edge tabs. This menu
    // item hides/shows BOTH columns at once; the checkmark = both hidden.
    bool bothHidden = m_leftPanelHidden && m_rightPanelHidden;
    if (ImGui::MenuItem(materializr::tr("Hide Panels"), "F9", bothHidden)) {
        bool hide = !bothHidden;
        m_leftPanelHidden = m_rightPanelHidden = hide;
        saveAppSettings();
    }
    ImGui::Separator();
    if (m_themeManager->renderSelector()) {
        m_themeManager->apply();
    }
}

void Application::renderHelpMenuItems() {
    if (ImGui::MenuItem(materializr::tr("User Guide"))) m_helpPanel->setVisible(true);
    if (ImGui::MenuItem(materializr::tr("Keyboard Shortcuts"))) m_shortcutsPanel->setVisible(true);
    ImGui::Separator();
    if (ImGui::MenuItem(materializr::tr("Check for Updates..."))) {
        m_showUpdatePopup = true;
        m_updateChecked = false; // run the network call when the popup opens
    }
    // Plugin-contributed Help items (e.g. the Tutorial's "Getting
    // Started"). Lets a plugin add a launcher without Application
    // knowing about it. See renderPluginMenuItems.
    renderPluginMenuItems("Help");
    ImGui::Separator();
    if (ImGui::MenuItem(materializr::tr("About Materializr..."))) m_aboutDialog->setVisible(true);
}

void Application::renderPluginMenuItems(const char* menuName) {
    // Render every plugin MenuContribution whose path is "<menuName> > Label"
    // as a MenuItem in the current menu. Keeps the contribution type generic
    // (a plugin says where it wants to live) without Application hardcoding it.
    auto trim = [](std::string s) {
        size_t a = s.find_first_not_of(" \t");
        size_t b = s.find_last_not_of(" \t");
        return (a == std::string::npos) ? std::string() : s.substr(a, b - a + 1);
    };
    for (auto& m : PluginRegistry::instance().menuContributions()) {
        auto gt = m.path.find('>');
        if (gt == std::string::npos) continue;
        if (trim(m.path.substr(0, gt)) != menuName) continue;
        std::string label = trim(m.path.substr(gt + 1));
        const bool enabled = !m.enabled || m.enabled(*m_pluginContext);
        const char* sc = m.shortcut.empty() ? nullptr : m.shortcut.c_str();
        if (ImGui::MenuItem(label.c_str(), sc, false, enabled) && m.action)
            m.action(*m_pluginContext);
    }
}

// The ⋯/☰ menu shared by the modern and im-touch layouts: the full desktop
// menus, flattened one level, via the shared item lists (renderFileMenuItems
// & co.) so these layouts and classic's menu bar cannot drift. Caller does
// OpenPopup("##TouchOverflow") on its trigger button.
void Application::renderTouchOverflowPopup() {
    if (!ImGui::BeginPopup("##TouchOverflow")) return;
    // The icon macro concatenates into the label, so these cannot be plain
    // literals for tr(); build icon + translated word + a ###pin so the menu's
    // ImGui identity survives a live language switch.
    auto menuLbl = [](const char* icon, const char* en) {
        static std::string buf;   // valid until the next call within this frame
        buf = std::string(icon) + "  " + materializr::tr(en) + "###" + en;
        return buf.c_str();
    };
    if (ImGui::BeginMenu(menuLbl(MZ_ICON_OPEN, "File"))) {
        renderFileMenuItems(false);   // Settings is exposed at the bottom instead
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(menuLbl(MZ_ICON_UNDO, "Edit"))) {
        renderEditMenuItems();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(menuLbl(MZ_ICON_FOCUS, "View"))) {
        renderViewMenuItems();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(menuLbl(MZ_ICON_ABOUT, "Help"))) {
        renderHelpMenuItems();
        ImGui::EndMenu();
    }
    ImGui::Separator();
    if (ImGui::MenuItem(menuLbl(MZ_ICON_SETTINGS, "Settings..."))) {
        m_settingsOrbitButton = m_orbitButton;
        m_settingsPanButton   = m_panButton;
        m_showSettings = true;
    }
    ImGui::EndPopup();
}

void Application::renderRailPolygonSidesPopup(bool clicked) {
    // Same side-count popout as the classic sketch toolbar (Toolbar.cpp): pick a
    // named polygon, which sets the tool's side count and starts placement - so
    // every layout drives the identical polygon flow. `clicked` is this frame's
    // rail-button result; the popup body renders every frame while open.
    if (clicked) ImGui::OpenPopup("##railPolySides");
    if (ImGui::BeginPopup("##railPolySides")) {
        struct PolyChoice { const char* name; int sides; };
        static const PolyChoice choices[] = {
            {"Triangle (3)", 3}, {"Square (4)", 4}, {"Pentagon (5)", 5},
            {"Hexagon (6)", 6}, {"Heptagon (7)", 7}, {"Octagon (8)", 8}};
        for (const auto& c : choices)
            if (ImGui::MenuItem(c.name)) {
                m_toolbar->setRequestedPolygonSides(c.sides);
                handleToolAction(static_cast<int>(ToolAction::Polygon));
            }
        ImGui::EndPopup();
    }
}

} // namespace materializr
