#pragma once
// Drop-in replacements for ImGui's numeric inputs.
//
// Signature-compatible with ImGui::InputDouble / InputFloat / InputInt on
// purpose: migrating a panel is a find-replace, not a rewrite. That matters
// because the alternative - the pattern the im-touch face-op panels use - is
// an ADDITIVE second call per field:
//
//     if (stepperRow("taperStep", &m_angle, true, -45, 45)) changed = true;
//     if (ctx.cornerCommitUi &&
//         touchui::amountField("taperAmt", nullptr, &m_angle, "deg", 1, ...))
//         changed = true;
//
// ...with its own invented id, units, decimals and range. Ten lines of
// judgement per field, which is why it reached 19 sites and stopped, while
// ~107 numeric fields kept raising the OS keyboard on a tablet.
//
// NUMERIC ONLY. Anything alphanumeric - Items-panel renames, variable
// expressions, body names - stays on ImGui::InputText and the native
// keyboard, which is the right tool for letters. The one thing to watch when
// converting a panel is a numeric field wearing a text coat: an InputText
// carrying ImGuiInputTextFlags_CharsDecimal (e.g. the sketch constraint value
// in PropertiesPanel) is a number, and a plain search for InputDouble misses
// it.
//
// Desktop is unchanged BY CONSTRUCTION: with touch mode off these forward
// straight to the ImGui call they replaced, same arguments, same return.

#include "TouchWidgets.h"
#include "../touch_mode.h"
#include "../ui_scale.h"
#include <imgui.h>
#include <imgui_internal.h>   // NextItemData: did the caller SetNextItemWidth?
#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace materializr {

// Width for the pad's collapsed well: the caller's SetNextItemWidth when there
// is one, else 0 (= the full row, numberField's default). Ignoring an explicit
// width wasn't just cosmetic: a full-row well followed by SameLine() (a unit
// suffix, the Offset dialog's Flip button) in an AlwaysAutoResize window grows
// the window by the trailing item every frame, straight off the screen.
inline float padWellWidth() {
    if (!(GImGui->NextItemData.HasFlags & ImGuiNextItemDataFlags_HasWidth)) return 0.0f;
    return std::max(ImGui::CalcItemWidth(), 70.0f * uiScale());
}

// Returns true when the value changed - per keystroke on desktop (ImGui's
// behaviour), once on Enter under the number pad. Callers that only set a
// dirty flag need no changes; a caller that live-previews every keystroke
// will preview on commit instead, which on a tablet is the better trade.
// `flags` exists for the dozen sites that pass EnterReturnsTrue. That flag is
// already the pad's semantics - it commits on Enter and nowhere else - so the
// touch branch ignores it and the desktop branch forwards it unchanged.
inline bool inputNumber(const char* label, double* v, double step = 0.1,
                        double stepFast = 1.0, const char* fmt = "%g",
                        ImGuiInputTextFlags flags = 0, bool* opened = nullptr) {
    if (!touchMode()) {
        const bool r = ImGui::InputDouble(label, v, step, stepFast, fmt, flags);
        // Desktop equivalent of the pad unfolding: the field took focus.
        if (opened && ImGui::IsItemActivated()) *opened = true;
        return r;
    }
    return touchui::numberField(label, label, v, fmt, opened, nullptr, padWellWidth());
}

inline bool inputNumber(const char* label, float* v, float step = 0.1f,
                        float stepFast = 1.0f, const char* fmt = "%g",
                        ImGuiInputTextFlags flags = 0) {
    if (!touchMode())
        return ImGui::InputFloat(label, v, step, stepFast, fmt, flags);
    double d = static_cast<double>(*v);
    if (!touchui::numberField(label, label, &d, fmt, nullptr, nullptr, padWellWidth()))
        return false;
    *v = static_cast<float>(d);
    return true;
}

inline bool inputNumberInt(const char* label, int* v, int step = 1,
                           int stepFast = 10) {
    if (!touchMode()) return ImGui::InputInt(label, v, step, stepFast);
    double d = static_cast<double>(*v);
    // "%.0f" keeps the pad's readout free of a trailing ".000" - the decimal
    // key still types one, but the commit truncates, same as InputInt.
    if (!touchui::numberField(label, label, &d, "%.0f", nullptr, nullptr, padWellWidth()))
        return false;
    *v = static_cast<int>(d);
    return true;
}

// The field inputNumberText() committed this frame, so lengthBufferIsActive()
// can report it as being edited for exactly that frame (see below).
struct NumberTextCommit { ImGuiID id = 0; int frame = -1; };
inline NumberTextCommit& numberTextCommit() { static NumberTextCommit c; return c; }
inline bool numberTextCommittedThisFrame(ImGuiID id) {
    const NumberTextCommit& c = numberTextCommit();
    return c.id == id && c.frame == ImGui::GetFrameCount();
}

// The same idea for a NUMERIC TEXT BUFFER - the op panels' "##dist"-style
// fields that keep text so a unit ("2in") can be typed, and the count/angle
// fields parsed with atoi/parseFinite. Signature-compatible with
// ImGui::InputText; desktop forwards to it unchanged.
//
// Touch mode gets the number pad over the buffer's current number. Its Enter
// writes the value back into `buf` as plain digits (still in the display unit
// the caller parses) and:
//  - marks the field active for this one frame, so a caller's "parse while
//    typing" branch (lengthBufferIsActive) takes the value and refreshes its
//    preview, exactly as it would for a keystroke;
//  - returns true, like a keystroke would, EXCEPT for an EnterReturnsTrue
//    field. There Enter means "commit the whole operation" (extrude, fillet,
//    shell...), and the pad's Enter only means "this is the number" - the
//    user still confirms with the panel's own button, as on im-touch. Pass
//    padEnterCommits for an Enter field with no live path, whose Enter really
//    is just "use this value" (the sketch's placement dimension).
inline bool inputNumberText(const char* label, char* buf, size_t bufSize,
                            ImGuiInputTextFlags flags = 0,
                            bool padEnterCommits = false) {
    if (!touchMode()) return ImGui::InputText(label, buf, bufSize, flags);
    // Never the full row here: these fields almost always have a unit suffix
    // on the same line (see padWellWidth). CalcItemWidth is the caller's
    // SetNextItemWidth, or ImGui's default item width when there is none.
    const float wellW = std::max(ImGui::CalcItemWidth(), 70.0f * uiScale());
    double v = std::strtod(buf, nullptr);
    if (!touchui::numberField(label, label, &v, "%g", nullptr, nullptr, wellW))
        return false;
    std::snprintf(buf, bufSize, "%.10g", v);
    numberTextCommit() = { ImGui::GetID(label), ImGui::GetFrameCount() };
    return padEnterCommits || !(flags & ImGuiInputTextFlags_EnterReturnsTrue);
}

} // namespace materializr
