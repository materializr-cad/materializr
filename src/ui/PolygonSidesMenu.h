#pragma once
// The body of the Polygon side-count pop-up, shared by the classic toolbar and
// the rail layouts so every layout offers the same choices: the common regular
// polygons in one click, and a Custom row for any other count.

#include "../i18n.h"
#include <imgui.h>
#include <algorithm>

namespace materializr {

inline constexpr int kPolygonSidesMin = 3;
// A polygon is that many line segments in the solver and the region builder,
// so the count is capped well short of "a circle drawn the hard way".
inline constexpr int kPolygonSidesMax = 128;

// Draws the pop-up's items. Returns the chosen side count (>= 3) on the frame a
// choice is made, else 0. Call between BeginPopup and EndPopup.
inline int polygonSidesMenuItems() {
    struct PolyChoice { const char* name; int sides; };
    static const PolyChoice choices[] = {
        {"Triangle (3)", 3}, {"Square (4)", 4}, {"Pentagon (5)", 5},
        {"Hexagon (6)", 6}, {"Heptagon (7)", 7}, {"Octagon (8)", 8}};
    int picked = 0;
    for (const auto& c : choices)
        if (ImGui::MenuItem(tr(c.name))) picked = c.sides;

    ImGui::Separator();
    // Remembered between openings, so a second custom polygon of the same kind
    // is one click.
    static int custom = 9;
    ImGui::TextUnformatted(tr("Custom..."));
    // Sized from the live frame height, not a fixed 110 px: InputInt's -/+
    // buttons are each one frame tall, and in touch mode (big frames) they
    // swallowed the whole fixed width, leaving the number with no room.
    {
        const ImGuiStyle& st = ImGui::GetStyle();
        const float buttons = 2.0f * (ImGui::GetFrameHeight() + st.ItemInnerSpacing.x);
        const float digits = ImGui::CalcTextSize("128").x + 2.0f * st.FramePadding.x;
        ImGui::SetNextItemWidth(std::max(110.0f, buttons + digits));
    }
    // Not EnterReturnsTrue: InputInt reports a +/- button press as "committed"
    // under that flag too, which started the polygon on the first click of +.
    // Enter is read directly instead, only while the number field has the
    // keyboard.
    ImGui::InputInt("##polySidesCustom", &custom, 1, 1);
    const bool enter = ImGui::GetIO().WantTextInput &&
                       (ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                        ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false));
    custom = std::clamp(custom, kPolygonSidesMin, kPolygonSidesMax);
    ImGui::SameLine();
    const bool draw = ImGui::Button(tr("Draw"));
    if (enter || draw) picked = custom;
    return picked;
}

} // namespace materializr
