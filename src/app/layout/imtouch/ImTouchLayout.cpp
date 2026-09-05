// im-touch layout (UiLayout::ImTouch — the name is an homage to ImGui):
// near-zero chrome. The viewport fills the whole work rect; everything else
// floats over it — project/selection chip (top-left), undo + keyboard + menu
// (top-right), the contextual tool catalogue on the left edge, the
// Fusion-style history timeline (bottom-center), a "+" create FAB
// (bottom-right), and an fps readout.
//
// Everything fundamental (menus, tool catalogue, history editing) is shared
// code — see layout/LayoutCommon.h for the keep-in-lockstep contract.

#include "app/Application.h"
#include "app/layout/LayoutCommon.h"
#include <algorithm>   // std::min — viewport-capped popup height
#include "core/Document.h"
#include "core/History.h"
#include "core/Operation.h"
#include "core/SelectionManager.h"
#include "modeling/DeleteOp.h"          // Items tree: body delete via History
#include "modeling/SketchEditOp.h"      // timeline: Apply cascade targets
#include "modeling/SketchTransformOp.h"
#include "ui/UiTheme.h"                 // viewportTopChromeBottom
#include "modeling/SketchTool.h"   // SketchToolMode for the select-mode gate
#include "plugin/PluginContext.h"
#include "ui/HistoryPanel.h"
#include "ui/Toolbar.h"       // ToolAction for the tool-bar entries
#include "ui/TouchIcons.h"
#include "ui/TouchTheme.h"
#include "ui/TouchWidgets.h"
#include "touch_mode.h"
#include "ui_scale.h"

#include <cfloat> // FLT_MAX (tool bar height constraint)
#include <cstring> // strncpy (rename buffers)
#include <imgui.h>
#include <memory>  // make_unique (DeleteOp)
#include <set>
#include <string>
#include "../../../i18n.h"
#include "../../../i18n.h"
#include "../../../i18n.h"
#include "../../../i18n.h"
#include "../../../i18n.h"
#include "../../../i18n.h"

namespace materializr {

namespace {

// Icon for a history step's box in the timeline, by op typeId. Reloaded
// steps (ReplayOp) report the ORIGINAL op's typeId, so they map the same.
const char* stepIcon(const std::string& t) {
    if (t == "extrude")                    return MZ_ICON_EXTRUDE;
    if (t == "pushpull" || t == "moveface" || t == "move_hole")
                                           return MZ_ICON_PUSHPULL;
    if (t == "revolve")                    return MZ_ICON_LATHE;
    if (t == "loft" || t == "sweep")       return MZ_ICON_SPLINE;
    if (t == "fillet")                     return MZ_ICON_FILLET;
    if (t == "chamfer")                    return MZ_ICON_CHAMFER;
    if (t == "shell")                      return MZ_ICON_SHELL;
    if (t == "boolean")                    return MZ_ICON_SUBTRACT;
    if (t == "split_body")                 return MZ_ICON_TRIM;
    if (t == "delete")                     return MZ_ICON_DELETE;
    if (t == "defeature")                  return MZ_ICON_REPAIR;
    if (t == "mirror")                     return MZ_ICON_MIRROR;
    if (t == "pattern")                    return MZ_ICON_PATTERN;
    if (t == "copy" || t == "duplicate_sketch")
                                           return MZ_ICON_COPY;
    if (t == "scale_face" || t == "resize_cylindrical" || t == "taper")
                                           return MZ_ICON_SCALE;
    if (t == "primitive")                  return MZ_ICON_PRIMITIVE;
    if (t == "thread")                     return MZ_ICON_THREAD;
    if (t == "construction_plane" || t == "construction_axis")
                                           return MZ_ICON_AXES;
    if (t == "sketchedit" || t == "combine_sketches" || t == "project_sketch")
                                           return MZ_ICON_SKETCH;
    if (t == "transform" || t == "axis_transform" || t == "plane_transform" ||
        t == "sketchtransform" || t == "align")
                                           return MZ_ICON_MOVE;
    return MZ_ICON_EDIT;
}

} // namespace

void Application::renderImTouchLayout() {
    touchui::Scope style;

    const float s = uiScale();
    auto tip = [&](const char* text) {   // same policy as the modern layout
        if (!m_showToolbarTooltips || !text) return;
        if (ImGui::BeginItemTooltip()) {
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 22.0f);
            ImGui::TextUnformatted(text);
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    };
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 wp = vp->WorkPos;
    const ImVec2 ws = vp->WorkSize;
    const float m = 12.0f * s; // float margin from the work-rect edges

    // Reserve the strip the top-left chips and top-right cluster occupy, so the
    // overlays drawn INSIDE the viewport window (the op banner, the toast) start
    // below them instead of underneath. Both clusters are one 44*s box tall and
    // sit at wp.y + m. See viewportTopChromeBottom.
    materializr::viewportTopChromeBottom() = wp.y + m + 44.0f * s;

    // Viewport underneath everything.
    m_touchVpX = wp.x;
    m_touchVpY = wp.y;
    m_touchVpW = ws.x;
    m_touchVpH = ws.y;

    // These overlays float ON TOP of the full-screen viewport window, which is
    // NoBringToFrontOnFocus (pinned to the back). They must NOT share that flag:
    // if they do, z-order falls to ImGui's persistent creation order — which is
    // fine when the app LAUNCHES straight into im-touch (overlays created
    // early), but when the user TOGGLES to it at runtime the viewport was
    // created first and stays in front, burying every overlay (the "invisible
    // shell" bug). Dropping the flag makes them ordinary foreground windows
    // that come to front on appearance, so they render above the back-pinned
    // viewport every time.
    const ImGuiWindowFlags kFloat =
        (layoutui::kShellWindowFlags & ~ImGuiWindowFlags_NoBringToFrontOnFocus) |
        ImGuiWindowFlags_AlwaysAutoResize;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, touchui::radius(14.0f * s));
    // No window borders on any of the floating overlays — the 1px frame reads
    // as a faint "ghost" rectangle around transparent windows (the +, the
    // chip, the buttons). Their rounded fill is the only chrome we want.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

    // ── Top-left: [Logo] [Menu] [Project name] — three INDIVIDUAL boxes of
    //    equal height (matching the top-right cluster's button boxes), on a
    //    fully transparent host window so each box carries its own fill.
    ImGui::SetNextWindowPos(ImVec2(wp.x + m, wp.y + m));
    ImGui::SetNextWindowBgAlpha(0.0f);
    if (ImGui::Begin("##LiteChip", nullptr, kFloat)) {
        const float bh = 44.0f * s;
        ImDrawList* dl = ImGui::GetWindowDrawList();

        // Logo box.
        {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            dl->AddRectFilled(p, ImVec2(p.x + bh, p.y + bh),
                              ImGui::GetColorU32(touchui::rowBg()),
                              touchui::radius(10.0f * s));
            const float lg = 26.0f * s;
            const ImVec2 c0(p.x + (bh - lg) * 0.5f, p.y + (bh - lg) * 0.5f);
            dl->AddImageRounded(layoutui::logoTexture(), c0,
                                ImVec2(c0.x + lg, c0.y + lg),
                                ImVec2(0, 0), ImVec2(1, 1),
                                IM_COL32_WHITE, touchui::radius(4.0f * s));
            ImGui::Dummy(ImVec2(bh, bh));
        }
        ImGui::SameLine(0.0f, 8.0f * s);

        // Menu box.
        if (touchui::iconButton("menu", MZ_ICON_MENU_BARS, bh))
            ImGui::OpenPopup("##TouchOverflow");
        tip(materializr::tr("Menu: file, edit, view, help and settings"));
        renderTouchOverflowPopup();
        ImGui::SameLine(0.0f, 8.0f * s);

        // Project-name box (name + selection summary, e.g. "mug.mzr · Face (1)").
        {
            std::string pn = projectDisplayName();
            std::string sel;
            if (m_selection && m_selection->hasSelection()) {
                const SelectionType t = m_selection->primaryType();
                int n = 0;
                for (const auto& e : m_selection->getSelection())
                    if (e.type == t) ++n;
                static const char* kNames[] = { "None", "Body", "Face", "Edge",
                                                "Vertex", "Sketch", "Region",
                                                "Plane", "Axis" };
                const int ti = static_cast<int>(t);
                if (ti > 0 && ti < 9) {
                    sel = std::string("  ·  ") + kNames[ti] +
                          " (" + std::to_string(n) + ")";
                }
            }
            // Tab affordance (Steve's popout): more than one open project adds
            // a chevron and the chip becomes the button that opens the tabs
            // sheet — no extra chrome when only one project is open (the chip
            // still opens the sheet then too, for "+ New Project").
            std::string chev = "  " ICON_IC_NAV_ARROW_DOWN;
            if (sessionDirty(m_activeSession)) pn += " \xe2\x80\xa2";
            const float padX = 14.0f * s;
            const ImVec2 tn = ImGui::CalcTextSize(pn.c_str());
            const ImVec2 tsl = sel.empty() ? ImVec2(0.0f, 0.0f)
                                           : ImGui::CalcTextSize(sel.c_str());
            const ImVec2 tc = ImGui::CalcTextSize(chev.c_str());
            const float bw = padX * 2.0f + tn.x + tsl.x + tc.x;
            const ImVec2 p = ImGui::GetCursorScreenPos();
            dl->AddRectFilled(p, ImVec2(p.x + bw, p.y + bh),
                              ImGui::GetColorU32(touchui::rowBg()),
                              touchui::radius(10.0f * s));
            dl->AddText(ImVec2(p.x + padX, p.y + (bh - tn.y) * 0.5f),
                        ImGui::GetColorU32(touchui::textPrimary()), pn.c_str());
            if (!sel.empty())
                dl->AddText(ImVec2(p.x + padX + tn.x, p.y + (bh - tsl.y) * 0.5f),
                            ImGui::GetColorU32(touchui::textDim()), sel.c_str());
            dl->AddText(ImVec2(p.x + padX + tn.x + tsl.x,
                               p.y + (bh - tc.y) * 0.5f),
                        ImGui::GetColorU32(touchui::textDim()), chev.c_str());
            if (ImGui::InvisibleButton("##projTabsChip", ImVec2(bw, bh)))
                ImGui::OpenPopup("##TouchTabs");
            renderTouchTabsSheet();
        }
    }
    ImGui::End();

    // ── Undo / keyboard / menu (top-right) ──────────────────────────────────
    ImGui::SetNextWindowPos(ImVec2(wp.x + ws.x - m, wp.y + m),
                            ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.0f);
    if (ImGui::Begin("##LiteTopRight", nullptr, kFloat)) {
        const float bh = 44.0f * s;
        // Multi-Select (moved off the bottom-left viewport bar): 3D selection and
        // sketch Select/move mode only.
        const bool showMulti = !m_inSketchMode ||
            (m_sketchTool && m_sketchTool->getMode() == SketchToolMode::Select);
        if (showMulti) {
            if (touchui::pillButton("multi", MZ_ICON_SELECT, materializr::tr("Multi"),
                                    m_multiSelectToggle))
                m_multiSelectToggle = !m_multiSelectToggle;
            tip(materializr::tr("Multi-select: add taps to the current selection\n(the touch equivalent of holding Ctrl)"));
            ImGui::SameLine(0.0f, 8.0f * s);
        }
        // Inference level, in sketch mode — the same live cycle the modern
        // layout puts in its top bar and the classic toolbar puts in its rail.
        // It IS in this layout's tool catalogue, but only inside the "More"
        // flyout at the BOTTOM of a dock that scrolls once the sketch tools
        // outgrow the screen, so on a tablet it sits below the fold and reads
        // as missing (Steve: "i cannot seem to find it"). Beside the snap pill
        // is where it belongs — the two are the same kind of drawing aid, and
        // this cluster never scrolls. Honours the same "show the toggle"
        // setting the other layouts gate on.
        if (m_inSketchMode && m_showInferenceToolbarToggle && m_sketchTool) {
            const char* infLbl = "Full";
            switch (m_sketchTool->getInferenceLevel()) {
                case SketchTool::InferenceLevel::Full:    infLbl = "Full";    break;
                case SketchTool::InferenceLevel::Reduced: infLbl = "Reduced"; break;
                case SketchTool::InferenceLevel::Off:     infLbl = "Off";     break;
                case SketchTool::InferenceLevel::Max:     infLbl = "Max";     break;
            }
            if (touchui::twoRowButton("inflvl", materializr::tr("Inference"), infLbl))
                handleToolAction(static_cast<int>(ToolAction::SketchCycleInference));
            tip(materializr::tr("Sketch inference level (snapping / guides)\nTap to cycle: Full \xE2\x86\x92 Reduced \xE2\x86\x92 Off \xE2\x86\x92 Max\nMax widens the catch ranges for fingertips."));
            ImGui::SameLine(0.0f, 8.0f * s);
        }
        // Snap-to-grid — the corner square's im-touch home (renderSnapWidget
        // skips itself in this layout). Label shows the current step; accent
        // fill while snap is on; tap opens the shared settings popup.
        {
            char snapLbl[16];
            std::snprintf(snapLbl, sizeof(snapLbl), "%.3g", m_sketchGridStep);
            if (touchui::pillButton("snap", MZ_ICON_GUIDES, snapLbl,
                                    m_snapToGrid))
                ImGui::OpenPopup("SnapSettings");
            tip(m_snapToGrid ? "Snap ON — tap for step / toggle"
                             : "Snap off — tap for step / toggle");
            renderSnapSettingsPopup();
            ImGui::SameLine(0.0f, 8.0f * s);
        }
        // Items (model tree) reveal/hide — moved up from the right-edge rail
        // button so the whole toggle row lives in one place.
        if (touchui::pillButton("items", MZ_ICON_ITEMS, nullptr,
                                m_imTouchTree)) {
            m_imTouchTree = !m_imTouchTree;
            saveAppSettings();
        }
        tip(m_imTouchTree ? "Hide the model tree"
                          : "Show the model tree (bodies, sketches, construction)");
        ImGui::SameLine(0.0f, 8.0f * s);
        const bool histLocked = anyInteractivePreviewActive();
        ImGui::BeginDisabled(histLocked || !touchCanUndo());
        if (touchui::iconButton("undo", MZ_ICON_UNDO, bh)) touchUndo();
        ImGui::EndDisabled();
        tip(materializr::tr("Undo (in a sketch: backs out the in-progress shape first)"));
        if (materializr::touchMode()) {
            ImGui::SameLine(0.0f, 8.0f * s);
            if (touchui::iconButton("kb", MZ_ICON_KEYBOARD, bh))
                m_softKeyboardForced = !m_softKeyboardForced;
            tip(materializr::tr("Toggle the on-screen keyboard"));
        }
        // (History has its own bottom toggle; the ⋯ menu lives on the
        // top-left chip.)
    }
    ImGui::End();

    // (The Items toggle now lives in the top cluster; railBtnW is still the
    // History toggle's size at the bottom.)
    const float railBtnW = 60.0f * s;

    // ── Transparent model tree (right edge) — the structure the modern
    //    layout's Items panel shows: visibility eye + name + tap-to-select,
    //    plus press-and-hold context menus (rename / delete / move-to-folder)
    //    and folder grouping, mirroring ItemsPanel but touch-native.
    m_imTouchTreeHovered = false;   // fed to the long-press gate every frame
    if (m_imTouchTree && m_document) {
        // Flush to the right edge, below the top cluster's reach.
        ImGui::SetNextWindowPos(
            ImVec2(wp.x + ws.x - m, wp.y + ws.y * 0.5f),
            ImGuiCond_Always, ImVec2(1.0f, 0.5f));
        const float treeW = 260.0f * s;
        ImGui::SetNextWindowSizeConstraints(ImVec2(treeW, 0),
                                            ImVec2(treeW, ws.y - 2.0f * m));
        ImGui::SetNextWindowBgAlpha(0.25f);   // Fusion-browser translucency
        if (ImGui::Begin("##LiteTree", nullptr,
                         kFloat & ~ImGuiWindowFlags_NoScrollbar)) {
            // Report hover (incl. while a row is the active item) so a
            // stationary long-press over the tree arms a synthetic right-click.
            m_imTouchTreeHovered = ImGui::IsWindowHovered(
                ImGuiHoveredFlags_RootAndChildWindows |
                ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
            // (Project name intentionally omitted here — the top-left chip
            // already shows it; dropping the browser's root row reclaims space.)

            // Selected ids per kind, collected once.
            std::set<int> selB, selS, selP, selA;
            if (m_selection)
                for (const auto& e : m_selection->getSelection()) {
                    if (e.type == SelectionType::Body   && e.bodyId   >= 0) selB.insert(e.bodyId);
                    if (e.type == SelectionType::Sketch && e.sketchId >= 0) selS.insert(e.sketchId);
                    if (e.type == SelectionType::Plane  && e.planeId  >= 0) selP.insert(e.planeId);
                    if (e.type == SelectionType::Axis   && e.axisId   >= 0) selA.insert(e.axisId);
                }
            // Plain tap = single-select; with the Multi toggle armed, bodies
            // toggle in/out of the selection (same semantics as the Items
            // panel's Ctrl+click). Body picks are navigation-only, exactly
            // like the Items panel's rows; other kinds select plainly.
            auto pick = [&](SelectionEntry entry, bool multiOk) {
                if (!m_selection) return;
                if (multiOk && m_multiSelectToggle) m_selection->toggleSelection(entry);
                else                                m_selection->select(entry);
                if (entry.type == SelectionType::Body)
                    m_selection->setNavigationOnly(true);
            };
            // Rename is a native-keyboard modal (raised after the tree). The
            // key is namespaced so kinds never collide (see Application.h).
            auto startRename = [&](int key, const std::string& cur) {
                m_imTouchRenameKey = key;
                std::strncpy(m_imTouchRenameBuf, cur.c_str(),
                             sizeof(m_imTouchRenameBuf) - 1);
                m_imTouchRenameBuf[sizeof(m_imTouchRenameBuf) - 1] = '\0';
                m_imTouchRenameOpen = true;
            };

            // One body row + its long-press context menu. Returns false when a
            // Delete made the surrounding id list stale (caller stops looping).
            // Hue-wheel colour popup (opened on a swatch tap). Returns the new
            // colour via `out` when the user drags it. Called inside the row's
            // PushID scope, so the popup id is unique per row.
            auto colorPopup = [&](const char* popupId, const char* title,
                                  glm::vec3 cur, glm::vec3& out) -> bool {
                bool changed = false;
                if (ImGui::BeginPopup(popupId)) {
                    ImGui::TextUnformatted(title);
                    ImGui::SetNextItemWidth(materializr::uiSz(220, 0).x);
                    if (ImGui::ColorPicker3(
                            "##pick", &cur.x,
                            ImGuiColorEditFlags_NoInputs |
                            ImGuiColorEditFlags_NoSidePreview |
                            ImGuiColorEditFlags_PickerHueWheel)) {
                        out = cur;
                        changed = true;
                    }
                    ImGui::EndPopup();
                }
                return changed;
            };

            auto renderBody = [&](int id) -> bool {
                bool gone = false;
                ImGui::PushID(id);
                bool visible = m_document->isBodyVisible(id);
                glm::vec3 bcol = m_document->getBodyColor(id);
                auto act = touchui::treeLeaf(
                    "body", MZ_ICON_BODY,
                    m_document->getBodyName(id).c_str(), &visible,
                    selB.count(id) > 0, &bcol.x);
                if (act.eyeToggled) {
                    m_document->setBodyVisible(id, visible);
                    // The viewport only filters hidden bodies when it rebuilds
                    // its meshes — without this the flag flips but the stale
                    // mesh keeps drawing (#37; desktop's ItemsPanel returns
                    // colorChanged for the same reason).
                    m_meshesDirty = true;
                    markDirty();
                }
                if (act.swatchClicked) ImGui::OpenPopup("bodyColor");
                glm::vec3 newCol;
                if (colorPopup("bodyColor", "Body colour",
                               m_document->getBodyColor(id), newCol)) {
                    m_document->setBodyColor(id, newCol);
                    m_meshesDirty = true;   // colour bakes into the render mesh
                    markDirty();
                }
                if (act.clicked) {
                    SelectionEntry e;
                    e.type = SelectionType::Body;
                    e.bodyId = id;
                    // Parity with ItemsPanel::makeEntry — downstream code
                    // (highlight outline, ops) expects the shape on the entry.
                    try { e.shape = m_document->getBody(id); } catch (...) {}
                    pick(e, /*multiOk=*/true);
                }
                if (act.rightClicked) ImGui::OpenPopup("bodyCtx");
                if (ImGui::BeginPopup("bodyCtx")) {
                    if (ImGui::MenuItem(materializr::tr("Rename")))
                        startRename(id, m_document->getBodyName(id));
                    if (ImGui::MenuItem(materializr::tr("Delete"))) {
                        if (m_history) {
                            auto op = std::make_unique<DeleteOp>();
                            op->setBodyId(id);
                            m_history->pushOperation(std::move(op), *m_document);
                        } else {
                            m_document->removeBody(id);
                        }
                        if (m_selection) m_selection->clear();
                        gone = true;
                    }
                    if (!gone && ImGui::BeginMenu(materializr::tr("Move to folder"))) {
                        if (m_document->getBodyFolder(id) >= 0 &&
                            ImGui::MenuItem(materializr::tr("(root — no folder)"))) {
                            m_document->setBodyFolder(id, -1);
                            markDirty();
                        }
                        for (int fid : m_document->getAllFolderIds())
                            if (ImGui::MenuItem(m_document->getFolderName(fid).c_str())) {
                                m_document->setBodyFolder(id, fid);
                                markDirty();
                            }
                        ImGui::Separator();
                        if (ImGui::MenuItem(materializr::tr("New folder…"))) {
                            m_imTouchNewFolderBodies = { id };
                            m_imTouchNewFolderName[0] = '\0';
                            m_imTouchNewFolderOpen = true;
                        }
                        ImGui::EndMenu();
                    }
                    ImGui::EndPopup();
                }
                ImGui::PopID();
                return !gone;
            };

            bool any = false;
            const auto bodyIds   = m_document->getAllBodyIds();
            const auto folderIds = m_document->getAllFolderIds();
            if (!bodyIds.empty() || !folderIds.empty()) {
                any = true;
                // Visible "+ Folder" pill on the header (its own hit area) —
                // the obvious way to make an empty folder; bodies join via a
                // row's Move-to-folder menu.
                bool addFolderClick = false;
                if (touchui::treeGroup("grpBodies", "Bodies",
                                       static_cast<int>(bodyIds.size()),
                                       m_imTouchTreeOpenBodies, nullptr,
                                       "+ Folder", &addFolderClick))
                    m_imTouchTreeOpenBodies = !m_imTouchTreeOpenBodies;
                if (addFolderClick) {
                    m_imTouchNewFolderBodies.clear();
                    m_imTouchNewFolderName[0] = '\0';
                    m_imTouchNewFolderOpen = true;
                }
                if (m_imTouchTreeOpenBodies) {
                    // 1) Folders, each with their member bodies indented.
                    bool bodiesStale = false;
                    for (int fid : folderIds) {
                        ImGui::PushID(2000000 + fid); // namespace off body ids
                        bool fvis = m_document->isFolderVisible(fid);
                        bool fexp = m_document->isFolderExpanded(fid);
                        glm::vec3 fcol = m_document->getFolderColor(fid);
                        auto fact = touchui::treeLeaf(
                            "folder", MZ_ICON_OPEN,
                            m_document->getFolderName(fid).c_str(), &fvis,
                            /*selected=*/false, &fcol.x);
                        if (fact.eyeToggled) {
                            m_document->setFolderVisible(fid, fvis);
                            m_meshesDirty = true;  // cascades to member bodies (#37)
                            markDirty();
                        }
                        if (fact.clicked)
                            m_document->setFolderExpanded(fid, !fexp);
                        if (fact.swatchClicked) ImGui::OpenPopup("folderColor");
                        glm::vec3 fNewCol;
                        if (colorPopup("folderColor", "Folder colour",
                                       m_document->getFolderColor(fid), fNewCol)) {
                            m_document->setFolderColor(fid, fNewCol); // cascades
                            m_meshesDirty = true;
                            markDirty();
                        }
                        if (fact.rightClicked) ImGui::OpenPopup("folderCtx");
                        bool folderGone = false;
                        if (ImGui::BeginPopup("folderCtx")) {
                            if (ImGui::MenuItem(materializr::tr("Rename")))
                                startRename(2000000 + fid,
                                            m_document->getFolderName(fid));
                            if (ImGui::MenuItem(materializr::tr("Delete folder (keeps bodies)"))) {
                                m_document->removeFolder(fid);
                                markDirty();
                                folderGone = true;
                            }
                            ImGui::EndPopup();
                        }
                        if (!folderGone && fexp) {
                            ImGui::Indent();
                            for (int bid : m_document->getBodiesInFolder(fid))
                                if (!renderBody(bid)) { bodiesStale = true; break; }
                            ImGui::Unindent();
                        }
                        ImGui::PopID();
                        if (folderGone || bodiesStale) break;
                    }
                    // 2) Root-level bodies (folderId == -1).
                    if (!bodiesStale)
                        for (int id : m_document->getBodiesInFolder(-1))
                            if (!renderBody(id)) break;
                }
            }
            const auto sketchIds = m_document->getAllSketchIds();
            if (!sketchIds.empty()) {
                any = true;
                if (touchui::treeGroup("grpSketches", "Sketches",
                                       static_cast<int>(sketchIds.size()),
                                       m_imTouchTreeOpenSketches))
                    m_imTouchTreeOpenSketches = !m_imTouchTreeOpenSketches;
                if (m_imTouchTreeOpenSketches)
                    for (int id : sketchIds) {
                        ImGui::PushID(id);
                        bool visible = m_document->isSketchVisible(id);
                        auto act = touchui::treeLeaf(
                            "sketch", MZ_ICON_SKETCH,
                            m_document->getSketchName(id).c_str(), &visible,
                            selS.count(id) > 0);
                        if (act.eyeToggled) m_document->setSketchVisible(id, visible);
                        if (act.clicked) {
                            SelectionEntry e;
                            e.type = SelectionType::Sketch;
                            e.sketchId = id;
                            pick(e, /*multiOk=*/false);
                        }
                        bool sgone = false;
                        if (act.rightClicked) ImGui::OpenPopup("sketchCtx");
                        if (ImGui::BeginPopup("sketchCtx")) {
                            // The sketch currently being drawn: renaming or
                            // deleting it out from under the tool that's still
                            // writing to it leaves m_activeSketchId stale and
                            // silently re-loses the geometry the mid-edit save
                            // just protected.
                            bool isBeingDrawn = m_inSketchMode && id == m_activeSketchId;
                            if (ImGui::MenuItem(materializr::tr("Rename"), nullptr, false, !isBeingDrawn))
                                startRename(1000000 + id,
                                            m_document->getSketchName(id));
                            if (ImGui::MenuItem(materializr::tr("Delete"), nullptr, false, !isBeingDrawn)) {
                                m_document->removeSketch(id);
                                if (m_selection) m_selection->clear();
                                sgone = true;
                            }
                            ImGui::EndPopup();
                        }
                        ImGui::PopID();
                        if (sgone) break;   // sketchIds now stale
                    }
            }
            const auto planeIds = m_document->getAllPlaneIds();
            const auto axisIds  = m_document->getAllAxisIds();
            if (!planeIds.empty() || !axisIds.empty()) {
                any = true;
                if (touchui::treeGroup(
                        "grpConstruction", "Construction",
                        static_cast<int>(planeIds.size() + axisIds.size()),
                        m_imTouchTreeOpenConstruction))
                    m_imTouchTreeOpenConstruction = !m_imTouchTreeOpenConstruction;
                if (m_imTouchTreeOpenConstruction) {
                    bool cgone = false;
                    for (int id : planeIds) {
                        ImGui::PushID(id + 100000); // avoid plane/axis id collisions
                        const auto* p = m_document->getPlane(id);
                        std::string label = p ? p->name
                                              : std::string("Plane ") + std::to_string(id);
                        bool visible = m_document->isPlaneVisible(id);
                        auto act = touchui::treeLeaf("plane", MZ_ICON_PLANE,
                                                     label.c_str(), &visible,
                                                     selP.count(id) > 0);
                        if (act.eyeToggled) m_document->setPlaneVisible(id, visible);
                        if (act.clicked) {
                            SelectionEntry e;
                            e.type = SelectionType::Plane;
                            e.planeId = id;
                            pick(e, /*multiOk=*/false);
                        }
                        if (act.rightClicked) ImGui::OpenPopup("planeCtx");
                        if (ImGui::BeginPopup("planeCtx")) {
                            if (ImGui::MenuItem(materializr::tr("Rename")))
                                startRename(4000000 + id, label);
                            if (ImGui::MenuItem(materializr::tr("Delete"))) {
                                m_document->removePlane(id);
                                if (m_selection) m_selection->clear();
                                cgone = true;
                            }
                            ImGui::EndPopup();
                        }
                        ImGui::PopID();
                        if (cgone) break;   // planeIds now stale
                    }
                    for (int id : axisIds) {
                        if (cgone) break;
                        ImGui::PushID(id + 200000);
                        const auto* a = m_document->getAxis(id);
                        std::string label = a ? a->name
                                              : std::string("Axis ") + std::to_string(id);
                        bool visible = m_document->isAxisVisible(id);
                        auto act = touchui::treeLeaf("axis", MZ_ICON_AXES,
                                                     label.c_str(), &visible,
                                                     selA.count(id) > 0);
                        if (act.eyeToggled) m_document->setAxisVisible(id, visible);
                        if (act.clicked) {
                            SelectionEntry e;
                            e.type = SelectionType::Axis;
                            e.axisId = id;
                            pick(e, /*multiOk=*/false);
                        }
                        if (act.rightClicked) ImGui::OpenPopup("axisCtx");
                        if (ImGui::BeginPopup("axisCtx")) {
                            if (ImGui::MenuItem(materializr::tr("Rename")))
                                startRename(5000000 + id, label);
                            if (ImGui::MenuItem(materializr::tr("Delete"))) {
                                m_document->removeAxis(id);
                                if (m_selection) m_selection->clear();
                                cgone = true;
                            }
                            ImGui::EndPopup();
                        }
                        ImGui::PopID();
                        if (cgone) break;   // axisIds now stale
                    }
                }
            }
            if (!any)
                ImGui::TextColored(touchui::textDim(), "%s", materializr::tr("Nothing here yet"));
        }
        ImGui::End();

        // ── Rename modal (native keyboard) — raised from any context "Rename".
        //    Decodes the namespaced key to route the committed name back.
        if (m_imTouchRenameOpen) {
            ImGui::OpenPopup("Rename##imtouch");
            m_imTouchRenameOpen = false;
            m_imTouchRenameFocus = true;
        }
        if (ImGui::BeginPopupModal("Rename##imtouch", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted(materializr::tr("Name:"));
            if (m_imTouchRenameFocus) {
                ImGui::SetKeyboardFocusHere();
                m_imTouchRenameFocus = false;
            }
            ImGui::SetNextItemWidth(uiSz(240, 0).x);
            bool committed = ImGui::InputText(
                "##rnbuf", m_imTouchRenameBuf, sizeof(m_imTouchRenameBuf),
                ImGuiInputTextFlags_EnterReturnsTrue |
                ImGuiInputTextFlags_AutoSelectAll);
            bool ok     = ImGui::Button(materializr::tr("Rename"), uiSz(110, 44));
            ImGui::SameLine();
            bool cancel = ImGui::Button(materializr::tr("Cancel"), uiSz(110, 44));
            if (committed || ok) {
                const int key = m_imTouchRenameKey;
                if (m_imTouchRenameBuf[0] != '\0' && key >= 0) {
                    // Key ranges mirror Application.h / ItemsPanel.
                    if (key >= 5000000)      m_document->setAxisName(key - 5000000, m_imTouchRenameBuf);
                    else if (key >= 4000000) m_document->setPlaneName(key - 4000000, m_imTouchRenameBuf);
                    else if (key >= 2000000) m_document->setFolderName(key - 2000000, m_imTouchRenameBuf);
                    else if (key >= 1000000) m_document->setSketchName(key - 1000000, m_imTouchRenameBuf);
                    else                     m_document->setBodyName(key, m_imTouchRenameBuf);
                    markDirty();
                }
                m_imTouchRenameKey = -1;
                ImGui::CloseCurrentPopup();
            } else if (cancel || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                m_imTouchRenameKey = -1;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // ── New-folder modal — raised from a body's Move-to-folder menu or the
        //    Bodies header. Creates the folder and drops the pending bodies in.
        if (m_imTouchNewFolderOpen) {
            ImGui::OpenPopup("New Folder##imtouch");
            m_imTouchNewFolderOpen = false;
            m_imTouchNewFolderFocus = true;
        }
        if (ImGui::BeginPopupModal("New Folder##imtouch", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted(materializr::tr("Folder name:"));
            if (m_imTouchNewFolderFocus) {
                ImGui::SetKeyboardFocusHere();
                m_imTouchNewFolderFocus = false;
            }
            ImGui::SetNextItemWidth(uiSz(240, 0).x);
            bool committed = ImGui::InputText(
                "##nfname", m_imTouchNewFolderName,
                sizeof(m_imTouchNewFolderName),
                ImGuiInputTextFlags_EnterReturnsTrue);
            bool create = ImGui::Button(materializr::tr("Create"), uiSz(110, 44));
            ImGui::SameLine();
            bool cancel = ImGui::Button(materializr::tr("Cancel"), uiSz(110, 44));
            if (committed || create) {
                if (m_imTouchNewFolderName[0] != '\0') {
                    int newId = m_document->addFolder(m_imTouchNewFolderName);
                    for (int bid : m_imTouchNewFolderBodies)
                        m_document->setBodyFolder(bid, newId);
                    markDirty();
                }
                m_imTouchNewFolderBodies.clear();
                ImGui::CloseCurrentPopup();
            } else if (cancel || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                m_imTouchNewFolderBodies.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    // ── Contextual tool bar — the same catalogue the modern layout's rail
    //    uses, floating on the LEFT edge, vertically centered. Sketch mode
    //    appends Finish/Exit pills below the tools. Tall catalogues (sketch
    //    mode on a landscape tablet) can exceed the work rect, so cap the
    //    height and let the bar scroll rather than run off-screen.
    ImGui::SetNextWindowPos(ImVec2(wp.x + m, wp.y + ws.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.0f, 0.5f));
    // The dock is vertically CENTRED, so it grows symmetrically from the
    // middle. Reserve the top-left (menu chip) and bottom-left (History
    // button) zones so a tall catalogue can't expand over them — cap the max
    // height to twice the smaller half-gap. Beyond that it scrolls.
    const float leftReserve = 96.0f * s; // History / chip button + margin
    const float dockMaxH = std::max(120.0f * s,
                                    ws.y - 2.0f * m - 2.0f * leftReserve);
    ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0),
                                        ImVec2(FLT_MAX, dockMaxH));
    ImGui::SetNextWindowBgAlpha(0.92f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, touchui::panelBg());
    if (ImGui::Begin("##LiteToolBar", nullptr,
                     kFloat & ~ImGuiWindowFlags_NoScrollbar)) {
        if (m_toolbar) {
            const auto tools = m_toolbar->railTools();
            // Fire one catalogue entry; `inPopup` closes the hosting group
            // popup after one-shot actions (the inference cycle stays open so
            // repeated taps can walk the levels, and Polygon opens its own
            // nested sides popup which closes the whole stack on pick).
            auto fire = [&](const Toolbar::RailTool& tool, bool clicked,
                            bool inPopup) {
                if (tool.pluginIndex >= 0) {
                    if (clicked) {
                        m_toolbar->fireRailPlugin(tool.pluginIndex);
                        if (inPopup) ImGui::CloseCurrentPopup();
                    }
                } else if (tool.action == ToolAction::Polygon) {
                    renderRailPolygonSidesPopup(clicked);
                } else if (clicked) {
                    handleToolAction(static_cast<int>(tool.action));
                    if (inPopup &&
                        tool.action != ToolAction::SketchCycleInference)
                        ImGui::CloseCurrentPopup();
                }
            };
            auto flatButton = [&](const Toolbar::RailTool& tool, int id) {
                ImGui::PushID(id); // labels can repeat across groups
                const bool clicked = touchui::railButton(
                    tool.label, tool.icon, tr(tool.label), tool.active, 64.0f * s);
                tip(materializr::tr(tool.tip));
                fire(tool, clicked, /*inPopup=*/false);
                ImGui::PopID();
            };
            // Group button + flyout: the button wears the group's ACTIVE
            // tool (icon, label, accent fill) so the current mode stays
            // readable while collapsed; tapping opens a grid of the members.
            auto group = [&](const char* id, const char* popupId,
                             const char* groupIcon, const char* groupLabel,
                             const char* groupTip,
                             const std::vector<const Toolbar::RailTool*>& members) {
                if (members.empty()) return;
                const Toolbar::RailTool* activeTool = nullptr;
                for (const auto* t : members)
                    if (t->active) { activeTool = t; break; }
                if (touchui::railButton(id,
                                        activeTool ? activeTool->icon : groupIcon,
                                        materializr::tr(activeTool ? activeTool->label
                                                                   : groupLabel),
                                        activeTool != nullptr, 64.0f * s))
                    ImGui::OpenPopup(popupId);
                tip(materializr::tr(groupTip));
                if (ImGui::BeginPopup(popupId)) {
                    int idx = 0;
                    for (const auto* t : members) {
                        if (idx % 4 != 0) ImGui::SameLine(0.0f, 6.0f * s);
                        ImGui::PushID(idx++);
                        const bool clicked = touchui::railButton(
                            t->label, t->icon, tr(t->label), t->active, 64.0f * s);
                        tip(t->tip);
                        fire(*t, clicked, /*inPopup=*/true);
                        ImGui::PopID();
                    }
                    ImGui::EndPopup();
                }
            };

            if (!m_inSketchMode) {
                // Body/feature context: most tools stay flat, but two flyouts
                // (matching modern's rail):
                //   Transform = Move + Rotate + Scale
                //   Multiply  = Duplicate + Mirror + patterns + splits
                // Everything else — including unrecognised future plugins —
                // renders in catalogue order so nothing silently vanishes.
                //   Repair    = Patch + Sew + Merge Faces + Remove Feature
                // Its members come from four different selection contexts, so
                // the flyout holds whichever are usable right now — and the
                // group button doesn't appear at all when none are.
                std::vector<const Toolbar::RailTool*> multiply, transform, repair;
                int railIdx = 0;
                for (const auto& t : tools) {
                    if (t.pluginIndex >= 0 && t.label) {
                        if (std::strcmp(t.label, "Linear") == 0 ||
                            std::strcmp(t.label, "Circular") == 0 ||
                            std::strcmp(t.label, "Duplicate") == 0) {
                            multiply.push_back(&t); continue;
                        }
                        if (std::strcmp(t.label, "Patch") == 0 ||
                            std::strcmp(t.label, "Sew") == 0) {
                            repair.push_back(&t); continue;
                        }
                    } else if (t.action == ToolAction::Mirror ||
                               t.action == ToolAction::Split) {
                        multiply.push_back(&t); continue;
                    } else if (t.action == ToolAction::Move ||
                               t.action == ToolAction::Rotate ||
                               t.action == ToolAction::Scale) {
                        transform.push_back(&t); continue;
                    } else if (t.action == ToolAction::MergeFaces ||
                               t.action == ToolAction::RemoveFace) {
                        repair.push_back(&t); continue;
                    }
                    flatButton(t, railIdx++);
                }
                group("transformGroup", "##bodyTransform", MZ_ICON_MOVE,
                      "Transform", "Move, rotate or scale the selection",
                      transform);
                group("multiplyGroup", "##bodyMultiply", MZ_ICON_COPY,
                      "Multiply", "Copy, mirror, pattern or split the selection",
                      multiply);
                group("repairGroup", "##bodyRepair", MZ_ICON_REPAIR,
                      "Repair", "Fix up geometry: fill an opening, stitch "
                      "surfaces into a solid, merge split faces, or take a "
                      "feature back off",
                      repair);
            } else {
                // Sketch mode: the flat catalogue is ~19 buttons — a screen
                // and a half of scrolling. The DRAWING tools stay flat (they're
                // the constantly-switched core of sketching, and Steve wants
                // them one tap away); the occasional tools collapse into two
                // Fusion-style groups — Modify (trim/copy/mirror/patterns +
                // sketch plugins) and More (guides/measure/look-at). Select,
                // the draw tools and the active tool's origin toggle render
                // in catalogue order, so anything unrecognised (future tools)
                // also lands flat and can't silently vanish from the rail.
                std::vector<const Toolbar::RailTool*> modify, aids, importers;
                int railIdx = 0;
                for (const auto& t : tools) {
                    if (t.pluginIndex >= 0) { modify.push_back(&t); continue; }
                    switch (t.action) {
                    // Bringing outside artwork in is its own act, distinct from
                    // drawing or modifying: SVG outlines and aerofoil sections
                    // share a group so neither is buried in the flat rail.
                    case ToolAction::SketchSvg:
                    case ToolAction::SketchAirfoil:
                        importers.push_back(&t); break;
                    case ToolAction::Trim:
                    case ToolAction::SketchOffset:
                    case ToolAction::SketchCopy:
                    case ToolAction::SketchMirror:
                    case ToolAction::SketchLinearPattern:
                    case ToolAction::SketchRadialPattern:
                        modify.push_back(&t); break;
                    case ToolAction::SketchCycleInference:
                    case ToolAction::Measure:
                    case ToolAction::LookAtSketch:
                        aids.push_back(&t); break;
                    default:
                        flatButton(t, railIdx++); break;
                    }
                }
                group("modifyGroup", "##sketchModify", MZ_ICON_TRIM, "Modify",
                      "Modify tools: trim, copy, mirror, linear and circular "
                      "patterns",
                      modify);
                group("importGroup", "##sketchImport", MZ_ICON_SVG, "Import",
                      "Bring outside artwork into the sketch: an SVG outline or "
                      "an aerofoil section",
                      importers);
                group("aidsGroup", "##sketchAids", MZ_ICON_MORE, "More",
                      "Drawing guides level, measure, look at the sketch plane",
                      aids);
            }
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();

    // ── Bottom-right corner: in a sketch, the commit actions — an accent
    //    ✓ Finish FAB with a smaller ✗ Discard beside it (gap so a Finish tap
    //    can't stray onto Discard). During a live ACTION (push/pull, extrude,
    //    fillet, shell, pattern, …) the same pair reads Apply/Cancel and
    //    drives the action — the op panels hide their own Confirm/Cancel
    //    rows while this corner hosts them (imTouchActionCorner()).
    //    Everywhere else, the "+" create FAB. Commit actions used to live at
    //    the bottom of the left tool bar, which coupled "done sketching" to
    //    transient tool picking and put the throw-it-away button 8 px under
    //    the tools; the corner FAB spot is the tablet thumb zone, and the +
    //    menu is dead weight mid-sketch anyway.
    ImGui::SetNextWindowPos(ImVec2(wp.x + ws.x - m, wp.y + ws.y - m),
                            ImGuiCond_Always, ImVec2(1.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.0f);
    if (m_inSketchMode) {
        if (ImGui::Begin("##LiteFab", nullptr, kFloat)) {
            const bool toolRunning = m_sketchTool && m_sketchTool->isPlacing();
            const float fabD = 56.0f * s;
            const float side = 44.0f * s;
            // ✗ first (left), vertically centred on the FAB.
            const float topY = ImGui::GetCursorPosY();
            ImGui::SetCursorPosY(topY + (fabD - side) * 0.5f);
            if (touchui::iconButton("exitSk", MZ_ICON_DISCARD, side)) {
                if (toolRunning)
                    m_sketchTool->onCancel();
                else
                    ImGui::OpenPopup("Discard sketch?"); // confirm — destructive
            }
            tip(toolRunning
                    ? "Cancel the in-progress shape"
                    : "Throw the sketch away and leave (asks to confirm)");
            ImGui::SameLine(0.0f, 18.0f * s); // deliberate gap: no stray discards
            ImGui::SetCursorPosY(topY);
            if (touchui::fab("finishSk", MZ_ICON_FINISH, fabD)) {
                if (m_sketchShapeConfirmPending && toolRunning) {
                    // A circle held for its ✗/✓ bubble: commit it as-released.
                    // (onConfirm would RESET the in-flight placement instead.)
                    recordSketchMutation([&]{
                        m_sketchTool->onMouseDown(m_sketchShapePendingPos,
                                                  false); });
                    m_sketchShapeConfirmPending = false;
                } else if (toolRunning)
                    recordSketchMutation([&]{ m_sketchTool->onConfirm(); });
                else
                    handleToolAction(static_cast<int>(ToolAction::FinishSketch));
            }
            tip(toolRunning
                    ? "Finish the current shape, keeping the points placed"
                    : "Finish the sketch and leave sketch mode");
            if (ImGui::BeginPopupModal("Discard sketch?", nullptr,
                                       ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::TextUnformatted(
                    materializr::tr("Leave the sketch and throw away its changes?"));
                ImGui::Spacing();
                const float bw = 150.0f * s;
                if (ImGui::Button(materializr::tr("Discard Sketch"), ImVec2(bw, 44.0f * s))) {
                    ImGui::CloseCurrentPopup();
                    handleToolAction(static_cast<int>(ToolAction::ExitSketchDiscard));
                }
                ImGui::SameLine();
                if (ImGui::Button(materializr::tr("Keep Editing"), ImVec2(bw, 44.0f * s)))
                    ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
        }
        ImGui::End();
    } else if (imTouchActionCorner()) {
        if (ImGui::Begin("##LiteFab", nullptr, kFloat)) {
            const float fabD = 56.0f * s;
            const float side = 44.0f * s;
            // ✗ first (left), vertically centred on the ✓ FAB — mirrors the
            // sketch pair so the corner always means confirm/discard.
            const float topY = ImGui::GetCursorPosY();
            ImGui::SetCursorPosY(topY + (fabD - side) * 0.5f);
            if (touchui::iconButton("cancelAct", MZ_ICON_DISCARD, side))
                cancelActiveAction();
            tip(materializr::tr("Cancel the action and discard its preview"));
            ImGui::SameLine(0.0f, 18.0f * s); // deliberate gap: no stray cancels
            ImGui::SetCursorPosY(topY);
            if (touchui::fab("applyAct", MZ_ICON_FINISH, fabD))
                confirmActiveAction();
            tip(materializr::tr("Apply the action"));
        }
        ImGui::End();
    } else {
        if (ImGui::Begin("##LiteFab", nullptr, kFloat)) {
            if (touchui::fab("create", MZ_ICON_ADD))
                ImGui::OpenPopup("##LiteCreate");
            tip(materializr::tr("Create: a sketch or a primitive solid"));
            if (ImGui::BeginPopup("##LiteCreate")) {
                // Mirror the modern rail's create logic instead of dumping every
                // option flat: sketch is contextual (on a picked face/plane if there
                // is one, else a "New Sketch" submenu of world planes), the five
                // primitives live under ONE "Primitive" submenu, and construction
                // geometry derives from the selection — so only the relevant, grouped
                // create tools show, matching the classic + modern layouts.
                const bool faceOrPlaneSel = m_selection &&
                    (m_selection->hasSelectedFaces() ||
                     m_selection->primaryType() == SelectionType::Plane);
                if (faceOrPlaneSel) {
                    if (ImGui::MenuItem(MZ_ICON_SKETCH "  Sketch on selection"))
                        handleToolAction(static_cast<int>(ToolAction::SketchOnFace));
                } else if (ImGui::BeginMenu(MZ_ICON_SKETCH "  New Sketch")) {
                    if (ImGui::MenuItem(materializr::tr("XY plane")))
                        handleToolAction(static_cast<int>(ToolAction::StartSketchXY));
                    if (ImGui::MenuItem(materializr::tr("XZ plane")))
                        handleToolAction(static_cast<int>(ToolAction::StartSketchXZ));
                    if (ImGui::MenuItem(materializr::tr("YZ plane")))
                        handleToolAction(static_cast<int>(ToolAction::StartSketchYZ));
                    ImGui::EndMenu();
                }
                if (m_pluginContext &&
                    ImGui::BeginMenu(MZ_ICON_PRIMITIVE "  Primitive")) {
                    if (ImGui::MenuItem(materializr::tr("Box")))
                        m_pluginContext->requestInteractiveOp(InteractiveOp::PrimitiveBox);
                    if (ImGui::MenuItem(materializr::tr("Cylinder")))
                        m_pluginContext->requestInteractiveOp(InteractiveOp::PrimitiveCylinder);
                    if (ImGui::MenuItem(materializr::tr("Sphere")))
                        m_pluginContext->requestInteractiveOp(InteractiveOp::PrimitiveSphere);
                    if (ImGui::MenuItem(materializr::tr("Cone")))
                        m_pluginContext->requestInteractiveOp(InteractiveOp::PrimitiveCone);
                    if (ImGui::MenuItem(materializr::tr("Torus")))
                        m_pluginContext->requestInteractiveOp(InteractiveOp::PrimitiveTorus);
                    ImGui::EndMenu();
                }
                if (ImGui::BeginMenu(MZ_ICON_FOCUS "  Construction")) {
                    renderConstructionMenuItems();
                    ImGui::EndMenu();
                }
                ImGui::EndPopup();
            }
        }
        ImGui::End();
    }

    // ── History: a bottom toggle whose REOPEN button sits exactly where its
    //    minimize chevron is (not up on the Items rail). The toggle is a fixed
    //    left-anchored button — a chevron to hide while the strip is open, the
    //    History clock to reopen while collapsed — and the Fusion-360-style
    //    step strip (tap a box for its properties popup: edit params, roll
    //    to it, toggle/delete) is a separate scrolling window to its right.
    //    Hidden in sketch mode: rolling the host body back under a live sketch
    //    is forbidden (see History::setUndoFloor).
    const int steps = m_history ? m_history->stepCount() : 0;
    const bool histAvail = !m_inSketchMode && m_document && m_history;
    const float histX   = wp.x + m;   // bottom-left corner (fps moved to top)
    const float histGap = 8.0f * s;
    // Last frame's measured strip height, so the toggle can be centred on the
    // strip's vertical middle — the strip window carries padding the
    // borderless button doesn't, so plain bottom-alignment left it sitting low.
    // The height persists once the strip has rendered, so the button holds that
    // same centred position when collapsed instead of snapping back down.
    static float s_histStripH = 0.0f;
    if (histAvail) {
        if (s_histStripH > 0.0f)
            ImGui::SetNextWindowPos(
                ImVec2(histX, (wp.y + ws.y - m) - s_histStripH * 0.5f),
                ImGuiCond_Always, ImVec2(0.0f, 0.5f));
        else
            ImGui::SetNextWindowPos(ImVec2(histX, wp.y + ws.y - m),
                                    ImGuiCond_Always, ImVec2(0.0f, 1.0f));
        ImGui::SetNextWindowBgAlpha(0.0f);   // the button draws its own solid fill
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        if (ImGui::Begin("##LiteHistoryToggle", nullptr, kFloat)) {
            // Clock icon + "History" label — mirrors the Items button, and
            // accent-fills while the timeline is open. The button is the same
            // open or closed, so the reopen state sits exactly where the
            // collapse state was.
            if (touchui::railButton("histToggle", MZ_ICON_HISTORY, materializr::tr("History"),
                                    m_imTouchTimeline, railBtnW, /*solid=*/true)) {
                m_imTouchTimeline = !m_imTouchTimeline;
                saveAppSettings();
            }
            tip(m_imTouchTimeline ? "Hide the history timeline"
                                  : "Show the history timeline");
        }
        ImGui::End();
        ImGui::PopStyleVar();
    }
    if (m_imTouchTimeline && histAvail) {
        const float stripX = histX + railBtnW + histGap;
        ImGui::SetNextWindowPos(ImVec2(stripX, wp.y + ws.y - m),
                                ImGuiCond_Always, ImVec2(0.0f, 1.0f));
        // Always reserve room for the WIDEST thing the bottom-right corner can
        // host — the ✗+gap+✓ Cancel/Apply pair (44+18+56 = 118dp) — not just
        // the single "+" create FAB (56dp). Sizing to the FAB let the expanded
        // strip run under the Cancel ✗ during a live action (drawn OVER it on
        // Android, UNDER on Linux, purely by z-order — #20). Reserving the pair
        // width unconditionally keeps the strip's right edge fixed instead of
        // popping in/out as the corner button changes. +20 = the FAB window's
        // own padding plus a small visual gap.
        const float fabClear = m + (44.0f + 18.0f + 56.0f + 20.0f) * s;
        ImGui::SetNextWindowSizeConstraints(
            ImVec2(0, 0), ImVec2((wp.x + ws.x - fabClear) - stripX, FLT_MAX));
        ImGui::SetNextWindowBgAlpha(0.92f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, touchui::panelBg());
        if (ImGui::Begin("##LiteTimeline", nullptr,
                         (kFloat & ~ImGuiWindowFlags_NoScrollbar) |
                             ImGuiWindowFlags_HorizontalScrollbar)) {
            s_histStripH = ImGui::GetWindowSize().y;  // for centring the toggle
            // Empty history still shows a hint so toggling History in a fresh
            // project doesn't look like it did nothing.
            if (steps == 0)
                ImGui::TextColored(touchui::textDim(), "%s", materializr::tr("History: no steps yet"));
            const int curr = m_history->currentStep();
            const int failedAt = m_history->lastReplayFailure();
            const bool histLocked = anyInteractivePreviewActive();
            const ImU32 amber = ImGui::GetColorU32(ImVec4(0.95f, 0.75f, 0.3f, 1.0f));
            const ImU32 red   = ImGui::GetColorU32(ImVec4(1.0f, 0.45f, 0.35f, 1.0f));

            // Auto-scroll the current step into view whenever history mutates
            // (new op, undo/redo, edit) — not on user scrolls.
            static unsigned s_seenRev = ~0u;
            const bool historyMoved = (s_seenRev != m_history->revision());

            bool wantOpen = false;
            for (int i = 0; i < steps; ++i) {
                const Operation* op = m_history->getStep(i);
                if (!op) continue;
                if (i > 0) ImGui::SameLine(0.0f, 6.0f * s);
                ImGui::PushID(i);
                ImU32 tint = 0;
                if (i == failedAt)          tint = red;
                else if (op->isReloaded())  tint = amber;
                const bool dim = (i > curr) || !op->isEnabled();
                std::string stepName = op->name();
                if (stepName.empty()) stepName = op->typeId();
                if (touchui::timelineBox("step", stepIcon(op->typeId()),
                                         i == curr, i == m_imTouchHistoryEdit,
                                         dim, tint, 0.0f, stepName.c_str())) {
                    m_imTouchHistoryEdit = (m_imTouchHistoryEdit == i) ? -1 : i;
                    wantOpen = (m_imTouchHistoryEdit == i);
                    // Drive the viewport's orange edited-element highlight.
                    if (m_historyPanel)
                        m_historyPanel->setEditingStep(m_imTouchHistoryEdit);
                }
                if (i == curr && historyMoved) ImGui::SetScrollHereX(0.5f);
                ImGui::PopID();
            }
            s_seenRev = m_history->revision();

            if (wantOpen) ImGui::OpenPopup("##LiteStepProps");
            // Sized to host an UNFOLDED number pad (TouchWidgets numberField),
            // not just a column of one-line fields: at the old 360x460 the pad
            // had to be a popup of its own to fit, which is what put a popup
            // inside this popup.
            //
            // Capped against the VIEWPORT, not a bare constant: a fixed height
            // that happens to exceed the screen just hides the bottom of the
            // dialog, which is how the pad's Enter key went missing the first
            // time. Screen height wins whenever it's the smaller of the two.
            const float vpH = ImGui::GetMainViewport()->Size.y;
            const float maxPopupH = std::min(760.0f * s, vpH * 0.88f);
            ImGui::SetNextWindowSizeConstraints(ImVec2(420.0f * s, 0),
                                                ImVec2(420.0f * s, maxPopupH));
            if (ImGui::BeginPopup("##LiteStepProps")) {
                const Operation* op =
                    (m_imTouchHistoryEdit >= 0 && m_imTouchHistoryEdit < steps)
                        ? m_history->getStep(m_imTouchHistoryEdit)
                        : nullptr;
                if (!op) {
                    ImGui::CloseCurrentPopup();
                } else {
                    const int i = m_imTouchHistoryEdit;
                    std::string detail = op->description();
                    if (detail.empty()) detail = op->name();
                    ImGui::TextColored(touchui::textPrimary(), "%d. %s",
                                       i + 1, detail.c_str());
                    if (!op->isEnabled())
                        ImGui::TextColored(touchui::textDim(), "%s", materializr::tr("Disabled"));
                    if (i > curr)
                        ImGui::TextColored(touchui::textDim(), "%s", materializr::tr("Undone \xE2\x80\x94 Go Here replays it."));
                    if (i == failedAt) {
                        ImGui::PushTextWrapPos(0.0f);
                        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "%s", materializr::tr("Couldn't recompute after an upstream change. Edit its parameters, fix the step before it, or delete it."));
                        ImGui::PopTextWrapPos();
                    }
                    ImGui::Separator();

                    if (op->isReloaded()) {
                        ImGui::PushTextWrapPos(0.0f);
                        ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.3f, 1.0f), "%s", materializr::tr("Restored from an older save \xE2\x80\x94 no editable parameters. Undo/redo still work."));
                        ImGui::PopTextWrapPos();
                    } else {
                        // The op's own parameter editor — identical widgets to
                        // the desktop History panel's Properties section.
                        // An EXPLICIT height, derived from the popup's own cap.
                        // A fill height (-footerH) reads better but is circular
                        // here: BeginPopup implies AlwaysAutoResize, so the
                        // popup sizes to the child while the child sizes to the
                        // popup, and the result was a short popup anchored
                        // halfway down the screen with its lower half off it.
                        const float footerH = 44.0f * s +
                                              ImGui::GetStyle().ItemSpacing.y * 2.0f;
                        const float headerH = 80.0f * s;   // title + separator
                        const float childH =
                            std::max(200.0f * s, maxPopupH - footerH - headerH);
                        ImGui::BeginChild("##props", ImVec2(0.0f, childH), true);
                        ImGui::PushItemWidth(
                            ImGui::CalcTextSize("0000000").x +
                            2.0f * (ImGui::GetFrameHeight() +
                                    ImGui::GetStyle().ItemInnerSpacing.x));
                        const_cast<Operation*>(op)->renderProperties();
                        ImGui::PopItemWidth();
                        ImGui::EndChild();
                        ImGui::BeginDisabled(histLocked);
                        if (ImGui::Button(materializr::tr("Apply Changes"),
                                          ImVec2(-1.0f, 44.0f * s))) {
                            // Same sequence as HistoryPanel: carry inline
                            // sketch-dimension edits into later snapshots
                            // FIRST, then a transactional replay, then cascade
                            // so bodies built from the sketch follow.
                            m_history->propagateSketchValueEdits(i, *m_document);
                            const bool applied = m_history->editStep(
                                i, *m_document, /*transactional=*/true);
                            m_meshesDirty = true;
                            if (applied) {
                                if (auto* se =
                                        dynamic_cast<const SketchEditOp*>(op)) {
                                    auto tgt = se->getTarget();
                                    int sid = tgt ? m_document->findSketchId(
                                                        tgt.get())
                                                  : -1;
                                    if (sid >= 0) cascadeFromSketchEdit(sid);
                                } else if (auto* st = dynamic_cast<
                                               const SketchTransformOp*>(op)) {
                                    if (st->getSketchId() >= 0)
                                        cascadeFromSketchEdit(st->getSketchId());
                                }
                            }
                        }
                        ImGui::EndDisabled();
                    }
                    ImGui::Separator();

                    const float bw = 104.0f * s;
                    ImGui::BeginDisabled(histLocked || i == curr);
                    if (ImGui::Button(materializr::tr("Go Here"), ImVec2(bw, 44.0f * s))) {
                        // Roll the model to this step (Fusion's marker drag).
                        // Progress guard: a failed replay mid-walk must not
                        // spin forever.
                        while (m_history->currentStep() > i) {
                            const int before = m_history->currentStep();
                            undoWithCascade();
                            if (m_history->currentStep() == before) break;
                        }
                        while (m_history->currentStep() < i) {
                            const int before = m_history->currentStep();
                            redoWithCascade();
                            if (m_history->currentStep() == before) break;
                        }
                        m_meshesDirty = true;
                    }
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    ImGui::BeginDisabled(histLocked);
                    if (ImGui::Button(op->isEnabled() ? "Disable" : "Enable",
                                      ImVec2(bw, 44.0f * s))) {
                        // In-place toggle — preserves base bodies the op
                        // modifies (replayAll's doc.clear() would drop them).
                        m_history->setStepEnabled(i, !op->isEnabled(),
                                                  *m_document);
                        m_meshesDirty = true;
                    }
                    ImGui::SameLine();
                    if (ImGui::Button(materializr::tr("Delete"), ImVec2(bw, 44.0f * s))) {
                        if (m_history->removeStep(i, *m_document)) {
                            m_imTouchHistoryEdit = -1;
                            if (m_historyPanel)
                                m_historyPanel->setEditingStep(-1);
                            ImGui::CloseCurrentPopup();
                        } else {
                            showToast(
                                "Can't delete: a later operation depends on it.");
                        }
                        m_meshesDirty = true;
                    }
                    ImGui::EndDisabled();
                }
                ImGui::EndPopup();
            } else if (m_imTouchHistoryEdit >= 0) {
                // Popup dismissed by tapping elsewhere — drop the edit state
                // (and the viewport highlight) with it.
                m_imTouchHistoryEdit = -1;
                if (m_historyPanel) m_historyPanel->setEditingStep(-1);
            }
        }
        ImGui::End();
        ImGui::PopStyleColor();
    }

    // ── fps readout — a small solid chip at the top-centre. Hidden entirely
    //    via Settings → Appearance → "Show FPS counter".
    if (m_showFps) {
        ImGui::SetNextWindowPos(ImVec2(wp.x + ws.x * 0.5f, wp.y + m),
                                ImGuiCond_Always, ImVec2(0.5f, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.92f);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, touchui::panelBg());
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(9.0f * s, 4.0f * s));
        if (ImGui::Begin("##LiteFps", nullptr, kFloat)) {
            ImGui::SetWindowFontScale(0.82f);   // smaller than the shell text
            ImGui::TextColored(touchui::textDim(), materializr::tr("%.0f fps"),
                               ImGui::GetIO().Framerate);
            ImGui::SetWindowFontScale(1.0f);
        }
        ImGui::End();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    }

    ImGui::PopStyleVar(2); // WindowRounding + WindowBorderSize
}

} // namespace materializr
