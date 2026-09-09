#include "UiTheme.h"
#include "ui_scale.h"
#include "ShortcutsPanel.h"
#include <imgui.h>
#include "../i18n.h"
#include "../i18n.h"

namespace materializr {

ShortcutsPanel::ShortcutsPanel() = default;

void ShortcutsPanel::setVisible(bool vis) {
    m_visible = vis;
}

bool ShortcutsPanel::isVisible() const {
    return m_visible;
}

namespace {
// One "Key | what it does" table. Every section is the same shape, and the
// old hand-rolled version drifted out of sync with the real handler - six
// bindings it advertised did not exist (Ctrl+C/Ctrl+V for a clipboard that
// was never written, and S/L/C/R for sketch tools that are toolbar-only,
// while R actually switches the gizmo to Scale). Keep this list checked
// against Application::handleShortcuts.
struct Binding { const char* keys; const char* action; };

void section(const char* title, const char* tableId,
             const Binding* rows, int count) {
    ImGui::Spacing();
    ImGui::TextColored(materializr::accentText(), "%s", title);
    ImGui::Separator();
    const ImGuiTableFlags flags = ImGuiTableFlags_Borders |
                                  ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable(tableId, 2, flags)) return;
    ImGui::TableSetupColumn(materializr::tr("Shortcut"), ImGuiTableColumnFlags_WidthFixed,
                            uiSz(170, 0).x);
    ImGui::TableSetupColumn(materializr::tr("Action"), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (int i = 0; i < count; ++i) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(rows[i].keys);
        // WRAPPED, not plain text: the window size is remembered per user in
        // imgui.ini, so a default width can't be relied on - anyone who
        // opened the old panel keeps its width and would just see the
        // descriptions clipped.
        ImGui::TableNextColumn(); ImGui::TextWrapped("%s", rows[i].action);
    }
    ImGui::EndTable();
}
} // namespace

void ShortcutsPanel::render() {
    if (!m_visible) return;

    ImGui::SetNextWindowSize(uiSz(580, 640), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Keyboard Shortcuts", &m_visible)) {
        ImGui::End();
        return;
    }

    static const Binding kFile[] = {
        {"Ctrl+S",        "Save (names it only the first time)"},
        {"Ctrl+O",        "Open a project"},
        {"Ctrl+I",        "Import STEP"},
        {"Ctrl+E",        "Export STEP"},
    };
    static const Binding kEdit[] = {
        {"Ctrl+Z",        "Undo"},
        {"Ctrl+Y",        "Redo"},
        {"Ctrl+D",        "Duplicate the selection in place"},
        {"Ctrl+A",        "Select all (sketch, body, or edges/faces)"},
        {"Delete",        "Delete the selection"},
    };
    static const Binding kTabs[] = {
        {"Ctrl+Tab",       "Next project tab"},
        {"Ctrl+Shift+Tab", "Previous project tab"},
    };
    static const Binding kView[] = {
        {"Home",          "Reset the camera"},
        {"F",             "Frame the selection (or everything, if none)"},
        {"F9",            "Hide / restore the side panels"},
        {"W / E / R",     "Gizmo: translate / rotate / scale"},
    };
    static const Binding kTools[] = {
        {"Enter",         "Commit the running tool (and typed value)"},
        {"Esc",           "Cancel the running tool or gizmo drag"},
    };
    static const Binding kSketch[] = {
        {"D",             "Dimension tool"},
        {"Backspace",     "Remove the last spline point or stamp"},
        {"Esc",           "Cancel the shape; again to leave the sketch"},
        {"Double-click",  "Finish the spline being drawn"},
        {"Right-click",   "Constraints menu (needs a selection)"},
    };
    static const Binding kMouse[] = {
        {"Middle-drag",       "Orbit"},
        {"Shift+Middle-drag", "Pan (always, whatever the buttons are)"},
        {"Right-drag",        "Pan"},
        {"Scroll",            "Zoom toward the cursor"},
        {"Left-click",        "Select a face or sketch region"},
        {"Double-click",      "Select the whole body"},
        {"Left-drag",         "Box-select (on empty space)"},
        {"Right-click",       "Context menu"},
    };
    static const Binding kTouch[] = {
        {"Tap",              "Select"},
        {"Double-tap",       "Select the whole body"},
        {"One-finger drag",  "Orbit, or drive the running tool"},
        {"Two-finger drag",  "Pan or pinch-zoom"},
        {"Long-press",       "Context menu"},
        {"Two-finger tap",   "Undo"},
        {"Three-finger tap", "Redo"},
    };

    section("File", "scFile", kFile, IM_ARRAYSIZE(kFile));
    section("Edit", "scEdit", kEdit, IM_ARRAYSIZE(kEdit));
    section("Tabs", "scTabs", kTabs, IM_ARRAYSIZE(kTabs));
    section("View", "scView", kView, IM_ARRAYSIZE(kView));
    section("While a tool is running", "scTools", kTools, IM_ARRAYSIZE(kTools));
    section("In a sketch", "scSketch", kSketch, IM_ARRAYSIZE(kSketch));
    ImGui::Spacing();
    ImGui::TextDisabled("%s", materializr::tr("The drawing tools (Line, Circle, Rectangle, Arc,"));
    ImGui::TextDisabled("%s", materializr::tr("Spline, Polygon, Trim, Offset) are on the toolbar only."));

    section("Mouse", "scMouse", kMouse, IM_ARRAYSIZE(kMouse));
    ImGui::Spacing();
    ImGui::TextDisabled("%s", materializr::tr("Orbit and pan buttons are configurable in"));
    ImGui::TextDisabled("%s", materializr::tr("Settings \xE2\x86\x92 Navigation."));

    section("Touch", "scTouch", kTouch, IM_ARRAYSIZE(kTouch));

    ImGui::End();
}

} // namespace materializr
