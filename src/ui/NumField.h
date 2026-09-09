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
#include <imgui.h>
#include <cstdio>

namespace materializr {

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
    return touchui::numberField(label, label, v, fmt, opened);
}

inline bool inputNumber(const char* label, float* v, float step = 0.1f,
                        float stepFast = 1.0f, const char* fmt = "%g",
                        ImGuiInputTextFlags flags = 0) {
    if (!touchMode())
        return ImGui::InputFloat(label, v, step, stepFast, fmt, flags);
    double d = static_cast<double>(*v);
    if (!touchui::numberField(label, label, &d, fmt)) return false;
    *v = static_cast<float>(d);
    return true;
}

inline bool inputNumberInt(const char* label, int* v, int step = 1,
                           int stepFast = 10) {
    if (!touchMode()) return ImGui::InputInt(label, v, step, stepFast);
    double d = static_cast<double>(*v);
    // "%.0f" keeps the pad's readout free of a trailing ".000" - the decimal
    // key still types one, but the commit truncates, same as InputInt.
    if (!touchui::numberField(label, label, &d, "%.0f")) return false;
    *v = static_cast<int>(d);
    return true;
}

} // namespace materializr
