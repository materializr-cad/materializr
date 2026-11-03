#pragma once
// Length widgets. Every one of these takes and returns MILLIMETRES; the
// display unit is applied on the way in and stripped on the way out, inside
// this header, and nowhere else. A panel that shows or edits a length uses one
// of these - never a bare ImGui::InputFloat / SliderFloat / Text with "mm" in
// the format string. That rule is what makes "only presentation converts" a
// property of the code rather than a hope.
//
// Shapes:
//   lengthField(label, &mm)                  numeric input (InputDouble/Float, or the pad)
//   lengthSlider(label, &mm, loMm, hiMm)     slider; value AND bounds converted together
//   lengthTextField(label, buf, n, &mm)      text input that accepts "2in", "50mm"
//   lengthText(fmt, mm)                      readout; fmt is a tr() key with one %s
//   trFormat(fmt, args...)                   translated printf that never truncates
//   fmtVec3(x, y, z)                         "(1.00, 2.00, 3.00) in"
//   amountLengthField(id, label, &mm, ...)   im-touch counterpart of lengthField
//
// The conversion logic itself lives in core/LengthEdit.h (no ImGui) so it is
// tested headless; this file only submits ImGui items and hands results over.

#include "../core/LengthEdit.h"
#include "../core/Units.h"
#include "../i18n.h"
#include "NumField.h"
#include "StepperRow.h"
#include "TouchWidgets.h"

#include <imgui.h>
#include <imgui_internal.h>   // GetActiveID - same include the viewport already uses

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <type_traits>

namespace materializr {

// ─── Readouts ────────────────────────────────────────────────────────────────

namespace detail {
// printf argument adapter: std::string -> c_str(); everything else forwarded.
// The static_assert keeps a stray float-length from reaching a %s.
inline const char* fmtArg(const std::string& s) { return s.c_str(); }
inline const char* fmtArg(const char* s)        { return s; }
inline int                fmtArg(int v)                { return v; }
inline long               fmtArg(long v)               { return v; }
inline long long          fmtArg(long long v)          { return v; }
inline unsigned           fmtArg(unsigned v)           { return v; }
inline unsigned long      fmtArg(unsigned long v)      { return v; }   // size_t on this platform
inline unsigned long long fmtArg(unsigned long long v) { return v; }
inline double             fmtArg(double v)             { return v; }
// A float here is a NON-length numeric printed by its own spec (a %.0f%%
// percentage, a count of turns). Lengths never arrive as raw numbers - they
// come pre-formatted as std::string - so promoting to double is correct.
inline double             fmtArg(float v)              { return v; }
template <class T> void fmtArg(const T&) {
    static_assert(sizeof(T) == 0, "trFormat: pass counts as int, quantities pre-formatted as std::string");
}
} // namespace detail

// tr(fmt) then printf with exact sizing: a counting pass, then a buffer of that
// size. Long translated sentences are never cut. Quantities arrive already
// formatted (fmtLength / fmtArea / fmtVec3) so catalogue keys carry only %s and
// %d - never a unit.
//
// The format is a runtime-translated string BY DESIGN, so -Wformat-security
// (an error on the Android build, via -Werror=format-security) is silenced for
// this one function: the catalogue keys are ours, not user input.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-security"
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
template <class... A>
inline std::string trFormat(const char* fmt, const A&... a) {
    const char* f = tr(fmt);
    const int n = std::snprintf(nullptr, 0, f, detail::fmtArg(a)...);
    if (n < 0) return f;
    std::string out(static_cast<size_t>(n) + 1, '\0');
    std::snprintf(out.data(), out.size(), f, detail::fmtArg(a)...);
    out.resize(static_cast<size_t>(n));
    return out;
}
#pragma GCC diagnostic pop

// Reseed a controller's own text buffer from its millimetre member, unless
// that field is being edited RIGHT NOW. Decided before the item is submitted,
// using the field's own id, so an external change - or a unit switch - shows
// this frame while a half-typed value is never clobbered.
//
// The hand-rolled controllers treated their buffer as the source of truth and
// re-parsed it into the model every frame. Two consequences, both silent:
// switching units left the OLD unit's text in the buffer to be reinterpreted
// in the new one (1.00 mm became 1.00 in = 25.4 mm), and a model value with
// more precision than the buffer's decimals was truncated to them just by
// opening the tool. The member is the truth; the buffer follows it.
// A stepper row whose buttons mean what their labels say. stepperRow adds its
// literal magnitudes (10 / 1 / 0.1) straight to a millimetre member, so beside
// a field reading "in" the button labelled +1 moved the value by 1 mm - 0.039
// in - and the mm min/max bounds shrank the usable range by the unit factor.
//
// Magnitudes come from the unit table as {10*step, step, 0.1*step}, which for
// millimetres is exactly {10, 1, 0.1} - the behaviour that was already there.
// Bounds stay millimetres and keep stepperRow's clamp semantics (a bound only
// stops motion TOWARDS it), so call sites pass what they always passed.
// `showZero` = false drops the 0 button, for a value where zero is not a useful
// reset (a primitive's size).
inline bool lengthStepperRow(const char* id, float* mm, bool allowNegative,
                             float minMm, float maxMm, float zeroMm = 0.0f,
                             bool showZero = true) {
    const double s = unitInfo(currentUnit()).step;
    const double mags[3] = { 10.0 * s, s, 0.1 * s };   // DISPLAY units
    bool changed = false;
    bool first = true;
    ImGui::PushID(id);
    const float h = std::max(ImGui::GetFrameHeight(), 34.0f);

    auto button = [&](const char* label) -> bool {
        if (!first) {
            // Wrap instead of overflowing: seven buttons are wider than a
            // narrow op panel, most of all at touch scale.
            const ImGuiStyle& st = ImGui::GetStyle();
            const float w = ImGui::CalcTextSize(label).x + 2.0f * st.FramePadding.x;
            const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
            if (ImGui::GetItemRectMax().x + st.ItemSpacing.x + w <= right) ImGui::SameLine();
        }
        first = false;
        return ImGui::Button(label, ImVec2(0.0f, h));
    };
    auto step = [&](const char* label, double deltaDisplay) {
        if (button(label)) {
            const float target = static_cast<float>(toMm(toDisplay(*mm) + deltaDisplay));
            *mm = steppedValue(*mm, target - *mm, minMm, maxMm);
            changed = true;
        }
    };

    char buf[24];
    if (allowNegative)
        for (double m : mags) {
            std::snprintf(buf, sizeof(buf), "-%g", m);
            step(buf, -m);
        }
    if (showZero) {
        char zbuf[24];
        std::snprintf(zbuf, sizeof(zbuf), "%g", toDisplay(zeroMm));
        if (button(zbuf)) { *mm = zeroMm; changed = true; }
    }
    for (int i = 2; i >= 0; --i) {
        std::snprintf(buf, sizeof(buf), "+%g", mags[i]);
        step(buf, mags[i]);
    }
    ImGui::PopID();
    return changed;
}

// For the double-valued members (primitive sizes and origins).
inline bool lengthStepperRow(const char* id, double* mm, bool allowNegative,
                             double minMm, double maxMm, double zeroMm = 0.0,
                             bool showZero = true) {
    float f = static_cast<float>(*mm);
    const bool changed = lengthStepperRow(id, &f, allowNegative, static_cast<float>(minMm),
                                          static_cast<float>(maxMm),
                                          static_cast<float>(zeroMm), showZero);
    if (changed) *mm = static_cast<double>(f);
    return changed;
}

namespace detail {
// ImGui CharFilter callback: refuse a digit that would add a decimal place the
// unit does not display (see decimalDigitRefused). UserData -> int maxDecimals.
inline int decimalCapFilter(ImGuiInputTextCallbackData* d) {
    if (d->EventFlag != ImGuiInputTextFlags_CallbackCharFilter) return 0;
    const int maxDecimals = *static_cast<const int*>(d->UserData);
    ImGuiInputTextState* st = ImGui::GetInputTextState(GImGui->ActiveId);
    if (!st) return 0;
    return decimalDigitRefused(st->GetText(), st->TextLen, st->GetCursorPos(),
                               st->GetSelectionStart(), st->GetSelectionEnd(),
                               maxDecimals, d->EventChar) ? 1 : 0;
}
} // namespace detail

// inputNumberText for a LENGTH: on desktop the field stops accepting digits past
// the current display unit's decimals (two in millimetres), so 0.005 cannot be
// typed only to be rounded to 0.01. The touch pad keeps its own entry.
inline bool lengthNumberText(const char* label, char* buf, size_t bufSize,
                             ImGuiInputTextFlags flags = 0, bool padEnterCommits = false) {
    if (touchMode()) return inputNumberText(label, buf, bufSize, flags, padEnterCommits);
    int decimals = unitInfo(currentUnit()).decimals;
    return ImGui::InputText(label, buf, bufSize,
                            flags | ImGuiInputTextFlags_CallbackCharFilter,
                            detail::decimalCapFilter, &decimals);
}

inline bool lengthBufferIsActive(const char* label) {
    const ImGuiID id = ImGui::GetID(label);
    // inputNumberText's pad commit counts as an edit for the frame it lands in.
    return ImGui::GetActiveID() == id || numberTextCommittedThisFrame(id);
}
inline void reseedLengthBufferIfIdle(const char* label, char* buf, size_t n, double mm) {
    if (!lengthBufferIsActive(label)) formatLengthDigits(buf, n, mm);
}


// One-quantity readout: lengthText("Length: %s", mm).
inline void lengthText(const char* fmt, double mm) {
    ImGui::TextUnformatted(trFormat(fmt, fmtLength(mm)).c_str());
}

// ─── Inputs ──────────────────────────────────────────────────────────────────

struct LengthEdit {
    bool changed; bool active;
    // So a converted `if (inputNumber(...))` keeps reading naturally.
    explicit operator bool() const { return changed; }
};

// Numeric length input. The display value is recomputed from mm EVERY frame and
// written back only when the widget reports a change - so an untouched value
// never drifts, and a unit switch shows correctly on the next frame. Step and
// precision come from the unit table; there are deliberately no step/fmt
// parameters to get wrong.
namespace detail {
// ImGui::InputDouble with the decimal cap (#160). InputDouble hands its text
// field no callback, so a third decimal could be typed and was then rounded away
// on commit; this is InputScalar's own layout (a text field plus -/+ step
// buttons, same ID scope) with decimalCapFilter on the text. Desktop only - the
// touch pad has its own entry. `active` reports the TEXT field, which is what a
// caller's IsItemActive() read after InputDouble gave.
inline bool cappedStepperInput(const char* label, double* v, double step, double stepFast,
                               const char* fmt, ImGuiInputTextFlags flags, int decimals,
                               bool* active) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems) { if (active) *active = false; return false; }
    ImGuiContext& g = *GImGui;
    ImGuiStyle& style = g.Style;
    char buf[64];
    std::snprintf(buf, sizeof buf, fmt, *v);

    bool changed = false;
    const float btn = ImGui::GetFrameHeight();
    ImGui::BeginGroup();
    ImGui::PushID(label);
    ImGui::SetNextItemWidth(std::max(1.0f, ImGui::CalcItemWidth() - (btn + style.ItemInnerSpacing.x) * 2));
    // As InputScalar: the data is compared, not the string, so no edit is marked
    // for an untouched value.
    g.NextItemData.ItemFlags |= ImGuiItemFlags_NoMarkEdited;
    ImGui::InputText("", buf, sizeof buf,
                     flags | ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_CharsScientific |
                         ImGuiInputTextFlags_CallbackCharFilter,
                     decimalCapFilter, &decimals);
    if (ImGui::IsItemEdited()) {
        const double parsed = std::strtod(buf, nullptr);
        if (parsed != *v) { *v = parsed; changed = true; }
    }
    if (active) *active = ImGui::IsItemActive();

    const ImVec2 backupPad = style.FramePadding;
    style.FramePadding.x = style.FramePadding.y;
    ImGui::PushItemFlag(ImGuiItemFlags_ButtonRepeat, true);
    const double inc = g.IO.KeyCtrl ? stepFast : step;
    ImGui::SameLine(0, style.ItemInnerSpacing.x);
    if (ImGui::ButtonEx("-", ImVec2(btn, btn))) { *v -= inc; changed = true; }
    ImGui::SameLine(0, style.ItemInnerSpacing.x);
    if (ImGui::ButtonEx("+", ImVec2(btn, btn))) { *v += inc; changed = true; }
    ImGui::PopItemFlag();
    style.FramePadding = backupPad;

    const char* labelEnd = ImGui::FindRenderedTextEnd(label);
    if (label != labelEnd) {
        ImGui::SameLine(0, style.ItemInnerSpacing.x);
        ImGui::TextEx(label, labelEnd);
    }
    ImGui::PopID();
    ImGui::EndGroup();
    if (changed) ImGui::MarkItemEdited(g.LastItemData.ID);
    return changed;
}
} // namespace detail

inline LengthEdit lengthField(const char* label, double* mm, ImGuiInputTextFlags flags = 0) {
    const UnitInfo& u = unitInfo(currentUnit());
    char fmt[8];
    std::snprintf(fmt, sizeof fmt, "%%.%df", u.decimals);
    double disp = toDisplay(*mm);
    bool changed, active;
    if (touchMode()) {
        changed = inputNumber(label, &disp, u.step, u.step * 10.0, fmt, flags);
        active  = ImGui::IsItemActive();   // valid: the item just submitted IS this field
    } else {
        changed = detail::cappedStepperInput(label, &disp, u.step, u.step * 10.0, fmt, flags,
                                             u.decimals, &active);
    }
    if (changed) *mm = lengthFieldCommit(disp);
    return { changed, active };
}

inline LengthEdit lengthField(const char* label, float* mm, ImGuiInputTextFlags flags = 0) {
    double d = static_cast<double>(*mm);
    const LengthEdit r = lengthField(label, &d, flags);
    if (r.changed) *mm = static_cast<float>(d);
    return r;
}

// Slider with mm bounds. Value and bounds are converted together (sliderShadow)
// so the usable range is the same in every unit.
inline bool lengthSlider(const char* label, double* mm, double minMm, double maxMm) {
    const UnitInfo& u = unitInfo(currentUnit());
    char fmt[8];
    std::snprintf(fmt, sizeof fmt, "%%.%df", u.decimals);
    SliderShadow s = sliderShadow(*mm, minMm, maxMm);
    float v = static_cast<float>(s.value);
    if (!ImGui::SliderFloat(label, &v, static_cast<float>(s.lo), static_cast<float>(s.hi), fmt))
        return false;
    *mm = lengthFieldCommit(v);
    return true;
}

inline bool lengthSlider(const char* label, float* mm, float minMm, float maxMm) {
    double d = static_cast<double>(*mm);
    if (!lengthSlider(label, &d, minMm, maxMm)) return false;
    *mm = static_cast<float>(d);
    return true;
}

// Text input that accepts a typed unit ("2in", "50mm", "3'") or a bare number in
// the current unit. `buf` is the caller's persistent buffer. Whether this field
// is active is decided BEFORE the item is submitted, from its stable id, so the
// buffer can be reseeded from the model while showing correctly this frame and
// is never clobbered mid-edit. Commits on Enter or on focus-out after an edit.
// Returns {changed, active}; on a committed parse failure the buffer is
// reseeded from the unchanged model value.
inline LengthEdit lengthTextField(const char* label, char* buf, size_t n, double* mm) {
    const ImGuiID id = ImGui::GetID(label);
    const bool active = (ImGui::GetActiveID() == id);
    reseedBuffer(buf, n, *mm, active);
    const bool entered = ImGui::InputText(label, buf, n, ImGuiInputTextFlags_EnterReturnsTrue);
    const bool committed = entered || ImGui::IsItemDeactivatedAfterEdit();
    if (!committed) return { false, ImGui::IsItemActive() };
    double out = 0.0;
    if (parseLength(buf, out)) { *mm = out; return { true, false }; }
    reseedBuffer(buf, n, *mm, false);
    return { false, false };
}

// im-touch: the amount well + pad. Same shadow discipline as lengthField, and
// the unit's own suffix and precision instead of the "mm"/3 literal.
inline bool amountLengthField(const char* id, const char* label, double* mm,
                              bool allowSign = false, double minMm = 0.0, double maxMm = 0.0,
                              const ImVec2* padPos = nullptr) {
    const UnitInfo& u = unitInfo(currentUnit());
    double disp = toDisplay(*mm);
    const bool changed = touchui::amountField(id, label, &disp, u.suffix, u.decimals, allowSign,
                                              toDisplay(minMm), toDisplay(maxMm), padPos);
    if (changed) *mm = lengthFieldCommit(disp);
    return changed;
}

inline bool amountLengthField(const char* id, const char* label, float* mm,
                              bool allowSign = false, float minMm = 0.0f, float maxMm = 0.0f,
                              const ImVec2* padPos = nullptr) {
    double d = static_cast<double>(*mm);
    if (!amountLengthField(id, label, &d, allowSign, minMm, maxMm, padPos)) return false;
    *mm = static_cast<float>(d);
    return true;
}

} // namespace materializr
