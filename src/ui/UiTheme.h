#pragma once
#include <imgui.h>
#include <algorithm>
#include <cstdarg>

namespace materializr {

// Screen-space Y of the BOTTOM of any floating top chrome, or 0 when the layout
// has none. Classic and modern dock their chrome above the viewport, so the
// viewport's own top edge is already clear; im-touch floats its logo / menu /
// project chips ON TOP of a full-screen viewport, and anything placed at a fixed
// offset inside that window lands underneath them - which is exactly what
// happened to the op banner ("EXTRUDE - drag in viewport…" read as "EXT… m,
// Escape to cancel", the half naming the gesture being the half that vanished).
//
// Reset to 0 each frame before the layout dispatch; raised by the layout that
// floats chrome. Read by viewportBanner and the transient toast.
inline float& viewportTopChromeBottom() {
    static float v = 0.0f;
    return v;
}

// A blue accent for headings / section titles that stays READABLE on the active
// theme: the long-standing light blue on a dark background, but a deep blue on a
// light one (where the light blue was nearly invisible). Derived from the live
// window-background luminance, so it tracks the current ImGui theme with no
// global state - just call it wherever a heading used the hard-coded light blue.
inline ImVec4 accentText() {
    const ImVec4 bg = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
    const float lum = 0.299f * bg.x + 0.587f * bg.y + 0.114f * bg.z;
    return (lum < 0.5f) ? ImVec4(0.60f, 0.80f, 1.00f, 1.0f)   // dark bg  → light blue
                        : ImVec4(0.10f, 0.34f, 0.70f, 1.0f);  // light bg → deep blue
}

// A muted grey for secondary / de-emphasised text (date headers, dimmed steps,
// "nothing here" notes) that stays READABLE on either theme - a light grey on a
// dark background, a dark grey on a light one. The hard-coded ~0.5 greys read
// fine on dark but washed out on light.
inline ImVec4 dimText() {
    const ImVec4 bg = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
    const float lum = 0.299f * bg.x + 0.587f * bg.y + 0.114f * bg.z;
    return (lum < 0.5f) ? ImVec4(0.62f, 0.62f, 0.66f, 1.0f)   // dark bg  → light grey
                        : ImVec4(0.38f, 0.38f, 0.42f, 1.0f);  // light bg → dark grey
}

// The coloured one-line hint every interactive tool draws at the top-left of
// the viewport ("PUSH/PULL - Positive = extrude, Negative = cut. …"). These
// were hand-rolled as SetCursorPos + PushStyleColor + Text at eight sites, and
// plain Text does not wrap: on a narrow viewport the sentence ran straight off
// the right edge, and the half that got clipped was always the tail - which is
// the half naming the confirm/cancel gesture (reported on a tablet, #71).
//
// Wrapping to the viewport's own width fixes it without touching the desktop
// look: a wide viewport still lays every one of these out on a single line, so
// this only ever changes what a too-narrow window does.
inline void viewportBanner(const ImVec4& col, const char* fmt, ...) {
    // Below the layout's floating chrome when there is any - see
    // viewportTopChromeBottom. Screen coords for that case, because the chrome's
    // position is screen-space and this window's own origin is not.
    const float chromeBottom = viewportTopChromeBottom();
    if (chromeBottom > 0.0f) {
        const ImVec2 wp = ImGui::GetWindowPos();
        ImGui::SetCursorScreenPos(
            ImVec2(wp.x + 10.0f, std::max(wp.y + 30.0f, chromeBottom + 10.0f)));
    } else {
        ImGui::SetCursorPos(ImVec2(10.0f, 30.0f));
    }
    ImGui::PushTextWrapPos(0.0f);   // 0 = wrap at the window's content edge
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    va_list args;
    va_start(args, fmt);
    ImGui::TextV(fmt, args);
    va_end(args);
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
}

} // namespace materializr
