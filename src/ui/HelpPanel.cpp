#include "UiTheme.h"
#include "ui_scale.h"
#include "HelpPanel.h"
#include <imgui.h>
#include "../i18n.h"

namespace materializr {

namespace {

// One section heading + a wrapped paragraph below it. Keeps the layout uniform.
void section(const char* title, const char* body) {
    ImGui::Spacing();
    ImGui::TextColored(materializr::accentText(), "%s", title);
    ImGui::Separator();
    ImGui::TextWrapped("%s", body);
    ImGui::Spacing();
}

} // namespace

void HelpPanel::render() {
    if (!m_visible) return;

    ImGui::SetNextWindowSize(uiSz(560, 540), ImGuiCond_FirstUseEver);
    if (m_raise) {
        ImGui::SetNextWindowFocus();
        m_raise = false;
    }
    if (!ImGui::Begin("User Guide", &m_visible)) { ImGui::End(); return; }

    ImGui::TextWrapped("%s", materializr::tr("Welcome to Materializr - a parametric 3D CAD app. This guide covers the basics so you can get something on screen quickly. Camera controls and key bindings can be changed in File → Settings."));

    section("Navigating the viewport",
        "Drag the middle mouse button to orbit, the right button to pan, and "
        "the scroll wheel to zoom. The cube in the top-right is a navigation "
        "widget: click a face for an orthographic view, click a corner dot for "
        "an isometric view, click an arrow for a 90° rotation, or drag the "
        "cube itself to free-orbit. Reset the camera with Home or "
        "View → Reset Camera.");

    section("Selecting things",
        "Single-click a face or edge to select it; Ctrl+click adds to the "
        "selection. Double-click selects the whole body. Drag in empty space "
        "to box-select multiple bodies at once. Esc clears or cancels the "
        "current operation.");

    section("Sketching",
        "Pick Sketch on XY/XZ/YZ from the Tools panel (or Sketch on Face after "
        "selecting one), then choose a tool: Line, Rectangle, Circle, Arc, "
        "Spline, Polygon, Trim, or Offset. Type a numeric dimension while "
        "placing to lock the size. Offset takes a whole connected chain: click "
        "one edge and the run it belongs to is highlighted, then move the "
        "cursor (the side you are on picks the direction) or type the "
        "distance. Press D for the Dimension tool: click a line, circle, "
        "two points, or two lines (parallel = distance, angled = angle), "
        "click to place the label, then type the value. Switch to Select / "
        "Move to drag existing points and lines - double-click empties to "
        "select the whole sketch, then use Copy / Mirror / Rotate. Click "
        "Finish Sketch (or press Enter) to exit.");

    section("Modelling from a sketch",
        "With a sketch region or face selected, click Extrude or Push/Pull. "
        "An arrow gizmo appears on the face - drag it for live preview, or "
        "type a value in the popup. Negative values cut into the body. After "
        "you have a body, use Fillet or Chamfer on edges (drag the handle "
        "outward to grow the radius/distance) and Mirror / Pattern from the "
        "tools sidebar.");

    section("Move / Rotate / Scale",
        "Select a body (or several) and pick the Move, Rotate or Scale gizmo "
        "from the toolbar. Drag a handle to transform. Rotate snaps softly to "
        "45°; Scale snaps to 1%% and shows a live readout. Scale has a "
        "side panel for typed per-axis values and a uniform-scale checkbox.");

    section("History",
        "Every modelling step goes into History on the right. Click a step to "
        "edit its parameters in Properties; right-click for Delete, Disable, "
        "Set Breakpoint, or Edit. Click a fillet/chamfer face in the viewport "
        "to jump straight to its history step. Reloaded projects show "
        "steps marked (reloaded) - undo/redo still walks through them but "
        "their parameters can't be re-edited.");

    section("Saving and autosave",
        "Ctrl+S saves; the project includes bodies, colours, sketches and the "
        "operation history. Settings → Autosave enables a periodic re-save "
        "once the project has a file on disk. Application preferences "
        "(theme, mouse buttons, orbit mode) are stored separately under your "
        "user config directory so they follow you between projects.");

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextDisabled("%s", materializr::tr("Tip: press Esc to cancel any in-progress operation. Ctrl+Z / Ctrl+Y undo and redo history steps."));

    ImGui::End();
}

} // namespace materializr
