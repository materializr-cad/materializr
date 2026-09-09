#pragma once
// Relative-nudge stepper row for op-preview dialogs (push/pull, extrude,
// fillet, chamfer, …) - a row of tap targets that replaces a fiddly slider.
//
// Each ± button ADDS its magnitude to *value; the "0" button zeroes it, which
// the op previews treat as "no change" (they guard against ~0 and fall back to
// the original body), so 0 removes the change mid-preview without leaving the
// dialog. `allowNegative` picks the button set:
//   signed  (push/pull, move, offset):  -10 -1 -0.1  0  +0.1 +1 +10
//   positive (fillet, chamfer, depth):        0  +0.1 +1 +10
// Results are clamped to [minV, maxV] EXCEPT the 0 button, which always sets
// exactly 0 so the "remove the change" fallback is reachable even when
// minV > 0. Returns true when *value changed this frame (caller re-previews).
//
// The clamp only ever stops a nudge that would LEAVE the range - it never drags
// a value that is already outside back in. The bounds exist so the buttons stay
// sane, not to police what you can type: the text field beside them accepts any
// value, and pulling an 80 mm extrude down to 50 because you pressed +1 loses
// what you typed and does the opposite of what the button says (Steve,
// 2026-08-03: "if i click the + or 1 numbers when something is past 50mm, it
// just goes to 50mm").

#include <imgui.h>
#include <algorithm>
#include <cstdio>

namespace materializr {

// One nudge, clamped. Split out from the button row so the rule is testable
// without an ImGui context - see tests/test_stepper_nudge.cpp.
//
// `before` outside [minV, maxV] is normal: the paired text field takes any
// value. In that case the bound in the direction of travel is not applied,
// because applying it would move the value BACKWARDS past where it started.
inline float steppedValue(float before, float delta, float minV, float maxV) {
    float v = before + delta;
    if (delta > 0.0f && before <= maxV) v = std::min(v, maxV);
    if (delta < 0.0f && before >= minV) v = std::max(v, minV);
    return v;
}

inline bool stepperRow(const char* id, float* value, bool allowNegative,
                       float minV, float maxV, float zeroValue = 0.0f) {
    static const float kMags[] = { 10.0f, 1.0f, 0.1f };
    bool changed = false;
    bool first = true;
    ImGui::PushID(id);
    const float h = std::max(ImGui::GetFrameHeight(), 34.0f);

    auto button = [&](const char* label) -> bool {
        if (!first) ImGui::SameLine();
        first = false;
        return ImGui::Button(label, ImVec2(0.0f, h));
    };
    auto step = [&](const char* label, float delta) {
        if (button(label)) {
            *value = steppedValue(*value, delta, minV, maxV);
            changed = true;
        }
    };

    char buf[16];
    if (allowNegative)
        for (float m : kMags) {
            std::snprintf(buf, sizeof(buf), "-%g", m);
            step(buf, -m);
        }
    {
        char zbuf[16];
        std::snprintf(zbuf, sizeof(zbuf), "%g", zeroValue);
        if (button(zbuf)) { *value = zeroValue; changed = true; }
    }
    for (int i = 2; i >= 0; --i) {   // +0.1, +1, +10 (ascending)
        std::snprintf(buf, sizeof(buf), "+%g", kMags[i]);
        step(buf, kMags[i]);
    }

    ImGui::PopID();
    return changed;
}

} // namespace materializr
