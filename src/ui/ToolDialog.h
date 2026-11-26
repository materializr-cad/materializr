#pragma once
#include <imgui.h>
#include <imgui_internal.h>   // GImGui->MovingWindow: "the user is dragging THIS window"
#include <algorithm>
#include <cmath>
#include <vector>
#include "ui/ToolDialogSpot.h"

namespace materializr {

// ONE place for every tool dialog (Loft, Push, Lathe, Shell, Mirror, ...).
//
// Each dialog used to pick its own spot - some a hard-coded 250-360 px in from the
// viewport's right edge (straight over the ViewCube), some centred at the top, a few
// never movable at all - so no two tools opened in the same place and the one you had
// just dragged out of the way was forgotten by the next. Now a dialog opens where the
// last one was left, in every layout. Until the user has dragged one, they open at a
// default that clears the ViewCube and the top of the view.
//
// Use: call toolDialogPlace(s) just before the dialog's Begin(), and
// toolDialogRemember(s) right after it. The dialog must be movable (no NoMove) and
// have a handle to move it by: ImGui's title bar, or opDialogDragGrip() when the
// window is title-less.
//
// What is remembered is the dialog's RIGHT edge relative to the viewport's right edge,
// and its top relative to the viewport's top, so a dialog of any width lines up with
// the last one and the spot survives resizing the window or hiding a panel.

// The viewport window's rect (screen px), refreshed every frame by the viewport.
struct ToolDialogHost {
    float x = 0, y = 0, w = 0, h = 0; float chromeBottom = 0;
    int lastSettleFrame = -10;   // last frame a dialog was still being lined up (see below)
};
inline ToolDialogHost& toolDialogHost() { static ToolDialogHost h; return h; }

// Space to keep clear at the viewport's top-right: the ViewCube and its buttons.
inline float toolDialogCubeClearance(float s) { return 172.0f * s; }

// Tool dialogs showing right now (a tool can open a second one - Construction Plane and its
// Move panel), so that a newcomer opens BESIDE them rather than on top.
struct ToolDialogLive { ImGuiID id; ImVec2 mn, mx; int frame; };
inline std::vector<ToolDialogLive>& toolDialogLive() { static std::vector<ToolDialogLive> v; return v; }

// Where a dialog's top-right corner goes right now. `selfId` is the dialog's own window
// (0 before it exists): the others that are open and reach into its row push it left.
inline ImVec2 toolDialogCorner(float s, ImGuiID selfId = 0) {
    const ToolDialogHost& h = toolDialogHost();
    const ToolDialogSpot& sp = toolDialogSpot();
    float rx, ty;
    if (sp.set) {
        rx = h.x + h.w + sp.rightOff;
        ty = h.y + sp.topOff;
    } else {
        rx = h.x + h.w - toolDialogCubeClearance(s);
        ty = h.y + 12.0f * s;
        // The im-touch top-right cluster floats over the view: start below it.
        if (h.chromeBottom > 0.0f) ty = std::max(ty, h.chromeBottom + 8.0f * s);
    }
    const int now = ImGui::GetFrameCount();
    for (const ToolDialogLive& o : toolDialogLive()) {
        if (o.id == selfId || o.frame < now - 1) continue;
        if (o.mx.y > ty && o.mn.y < ty + 40.0f * s && o.mx.x > rx - 40.0f * s)
            rx = std::min(rx, o.mn.x - 8.0f * s);
    }
    // Always inside the viewport (it may have shrunk since the spot was saved).
    rx = std::min(std::max(rx, h.x + 160.0f * s), h.x + h.w);
    ty = std::min(std::max(ty, h.y), h.y + std::max(0.0f, h.h - 80.0f * s));
    return ImVec2(rx, ty);
}

// Before Begin(): a dialog that is just appearing goes to the shared spot. It is
// ImGuiCond_Appearing on purpose, so a dialog the user is dragging is never snapped back.
inline void toolDialogPlace(float s) {
    ImGui::SetNextWindowPos(toolDialogCorner(s), ImGuiCond_Appearing, ImVec2(1.0f, 0.0f));
}

// After Begin(): (1) on its first frames a dialog is lined up on the shared spot by its
// real right edge - an auto-sizing window has no size yet when it is first placed, so
// the placement made before Begin() can only be approximate; (2) when the user is
// dragging the dialog, its position becomes the spot. `align` is false for a dialog that
// is deliberately anchored elsewhere (im-touch: beside the handle being dragged).
inline void toolDialogRemember(float s, bool align = true) {
    ImGuiContext& g = *ImGui::GetCurrentContext();
    const ToolDialogHost& h = toolDialogHost();
    if (h.w <= 0.0f) return;
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    const bool moving = (g.MovingWindow != nullptr && g.MovingWindow == win);
    const ImVec2 sz = ImGui::GetWindowSize();

    ImGuiStorage* st = ImGui::GetStateStorage();
    const ImGuiID key = ImGui::GetID("##toolDialogAge");
    const int age = ImGui::IsWindowAppearing() ? 0 : st->GetInt(key, 99);
    if (align && age < 3 && !moving) {
        const ImVec2 c = toolDialogCorner(s, win->ID);
        ImGui::SetWindowPos(ImVec2(std::max(c.x - sz.x, h.x), c.y));
        st->SetInt(key, age + 1);
        // The app draws on demand: ask for frames until the dialog has settled, or the
        // last one drawn is the half-moved one (its first row clipped).
        toolDialogHost().lastSettleFrame = ImGui::GetFrameCount();
    } else if (age < 99) {
        st->SetInt(key, 99);
    }

    {   // Register where this dialog is, for the ones that open after it.
        const ImVec2 q = ImGui::GetWindowPos();
        auto& live = toolDialogLive();
        const int now = ImGui::GetFrameCount();
        bool found = false;
        for (auto& o : live)
            if (o.id == win->ID) { o.mn = q; o.mx = ImVec2(q.x + sz.x, q.y + sz.y); o.frame = now; found = true; break; }
        if (!found) live.push_back({win->ID, q, ImVec2(q.x + sz.x, q.y + sz.y), now});
        live.erase(std::remove_if(live.begin(), live.end(),
                                  [now](const ToolDialogLive& o) { return o.frame < now - 120; }),
                   live.end());
    }

    if (!moving) return;
    const ImVec2 p = ImGui::GetWindowPos();
    ToolDialogSpot& sp = toolDialogSpot();
    sp.set = true;
    sp.rightOff = std::min(0.0f, (p.x + sz.x) - (h.x + h.w));
    sp.topOff = p.y - h.y;
}

} // namespace materializr
