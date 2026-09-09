#pragma once
#include <imgui.h>

namespace materializr {

// A slim draggable grip for the borderless op dialogs (push/pull, extrude,
// fillet/chamfer, ...): a faint centred "..." strip at the top that moves the
// window when dragged, so a panel that opened in an awkward spot can be nudged
// aside without a full title bar. Call right after Begin(); the window must have
// NoMove removed (grabbing the strip triggers ImGui's built-in move) and be
// positioned with ImGuiCond_Appearing so a drag isn't re-stomped to the anchor
// every frame.
inline void opDialogDragGrip(float s) {
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = 22.0f * s;   // generous strip: an easy touch target
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    // Reserve the strip as INERT space (Dummy uses id 0, so it never claims
    // HoveredId): grabbing it triggers ImGui's OWN window move - the dialogs
    // have NoMove removed. Built-in move handles fast flicks and edge-clamping
    // cleanly; the earlier manual SetWindowPos drag stalled slow and snapped
    // back on quick moves.
    ImGui::Dummy(ImVec2(w, h));
    const bool hov = ImGui::IsMouseHoveringRect(p0, ImVec2(p0.x + w, p0.y + h)) &&
                     ImGui::IsWindowHovered();
    if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 col = ImGui::GetColorU32(ImGuiCol_Text, hov ? 0.6f : 0.30f);
    const float cx = p0.x + w * 0.5f, cy = p0.y + h * 0.5f;
    const float r = 1.5f * s, gap = 5.5f * s;
    for (int i = -1; i <= 1; ++i)
        dl->AddCircleFilled(ImVec2(cx + i * gap, cy), r, col);
}

} // namespace materializr
