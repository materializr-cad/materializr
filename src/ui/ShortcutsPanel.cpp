#include "UiTheme.h"
#include "ui_scale.h"
#include "ShortcutsPanel.h"
#include <imgui.h>
#include <string>
#include <vector>
#include "../app/KeyBindings.h"
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
// while R actually switches the gizmo to Scale). The rebindable shortcuts now
// come straight from KeyBindings (the same table Application::handleShortcuts
// reads), so they cannot drift; the fixed rows below are the ones that are not
// configurable, and are checked against handleShortcuts by hand.
struct Row { std::string keys; std::string action; };

// A fixed row: both halves are catalogue keys (a chord such as "Ctrl+Click"
// is translated as one string, like the rest of the UI).
Row fixedRow(const char* keys, const char* action) {
    return { tr(keys), tr(action) };
}

// The rebindable shortcuts of one KeyBindings group, as rows.
void addBound(std::vector<Row>& rows, const KeyBindings* kb, const char* group) {
    if (!kb) return;
    for (int i = 0; i < KeyBindings::count(); ++i) {
        const KeyAction a = KeyBindings::fromIndex(i);
        const KeyActionInfo& inf = KeyBindings::info(a);
        if (std::string(inf.group) != group) continue;
        const std::string keys = KeyBindings::format(kb->chord(a), /*localized=*/true);
        rows.push_back({ keys.empty() ? std::string(tr("Unassigned")) : keys,
                         tr(inf.label) });
    }
}

void section(const char* title, const char* tableId, const std::vector<Row>& rows) {
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
    for (const Row& r : rows) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextWrapped("%s", r.keys.c_str());
        // WRAPPED, not plain text: the window size is remembered per user in
        // imgui.ini, so a default width can't be relied on - anyone who
        // opened the old panel keeps its width and would just see the
        // descriptions clipped.
        ImGui::TableNextColumn(); ImGui::TextWrapped("%s", r.action.c_str());
    }
    ImGui::EndTable();
}
} // namespace

void ShortcutsPanel::render() {
    if (!m_visible) return;

    ImGui::SetNextWindowSize(uiSz(580, 640), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(tr("Keyboard Shortcuts###KeyboardShortcuts"), &m_visible)) {
        ImGui::End();
        return;
    }

    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", tr("Keyboard shortcuts can be changed in Settings \xE2\x86\x92 Navigation."));
    ImGui::PopStyleColor();

    std::vector<Row> file, edit, tabs, view, tools, sketch, mouse, touch;
    addBound(file, m_bindings, "File");
    addBound(edit, m_bindings, "Edit");
    addBound(tabs, m_bindings, "Tabs");
    addBound(view, m_bindings, "View");
    tools = {
        fixedRow("Enter", "Commit the running tool (and typed value)"),
        fixedRow("Esc",   "Cancel the running tool or gizmo drag"),
    };
    addBound(sketch, m_bindings, "Sketch");
    sketch.push_back(fixedRow("Backspace",    "Remove the last spline point or stamp"));
    sketch.push_back(fixedRow("Esc",          "Cancel the shape; again for Select; again to leave the sketch"));
    sketch.push_back(fixedRow("Double-click", "Finish the spline being drawn"));
    sketch.push_back(fixedRow("Right-click",  "Constraints menu (needs a selection)"));
    mouse = {
        fixedRow("Middle-drag",       "Orbit"),
        fixedRow("Shift+Middle-drag", "Pan (always, whatever the buttons are)"),
        fixedRow("Right-drag",        "Pan"),
        fixedRow("Scroll",            "Zoom toward the cursor"),
        fixedRow("Left-click",        "Select a face or sketch region"),
        fixedRow("Double-click",      "Select the whole body"),
        fixedRow("Left-drag",         "Box-select (on empty space)"),
        fixedRow("Right-click",       "Context menu"),
    };
    touch = {
        fixedRow("Tap",              "Select"),
        fixedRow("Double-tap",       "Select the whole body"),
        fixedRow("One-finger drag",  "Orbit, or drive the running tool"),
        fixedRow("Two-finger drag",  "Pan or pinch-zoom"),
        fixedRow("Long-press",       "Context menu"),
        fixedRow("Two-finger tap",   "Undo"),
        fixedRow("Three-finger tap", "Redo"),
    };

    section(tr("File"), "scFile", file);
    section(tr("Edit"), "scEdit", edit);
    section(tr("Tabs"), "scTabs", tabs);
    section(tr("View"), "scView", view);
    section(tr("While a tool is running"), "scTools", tools);
    section(tr("In a sketch"), "scSketch", sketch);
    ImGui::Spacing();
    ImGui::TextDisabled("%s", materializr::tr("The drawing tools (Line, Circle, Rectangle, Arc,"));
    ImGui::TextDisabled("%s", materializr::tr("Spline, Polygon, Trim, Offset) are on the toolbar only."));

    section(tr("Mouse"), "scMouse", mouse);
    ImGui::Spacing();
    ImGui::TextDisabled("%s", materializr::tr("Orbit and pan buttons are configurable in"));
    ImGui::TextDisabled("%s", materializr::tr("Settings \xE2\x86\x92 Navigation."));

    section(tr("Touch"), "scTouch", touch);

    ImGui::End();
}

} // namespace materializr
