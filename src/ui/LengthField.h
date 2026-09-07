#pragma once
// Length widgets. Every one of these takes and returns MILLIMETRES; the
// display unit is applied on the way in and stripped on the way out, inside
// this header, and nowhere else. A panel that shows or edits a length uses one
// of these — never a bare ImGui::InputFloat / SliderFloat / Text with "mm" in
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
#include <imgui_internal.h>   // GetActiveID — same include the viewport already uses

#include <cstdio>
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
// percentage, a count of turns). Lengths never arrive as raw numbers — they
// come pre-formatted as std::string — so promoting to double is correct.
inline double             fmtArg(float v)              { return v; }
template <class T> void fmtArg(const T&) {
    static_assert(sizeof(T) == 0, "trFormat: pass counts as int, quantities pre-formatted as std::string");
}
} // namespace detail

// tr(fmt) then printf with exact sizing: a counting pass, then a buffer of that
// size. Long translated sentences are never cut. Quantities arrive already
// formatted (fmtLength / fmtArea / fmtVec3) so catalogue keys carry only %s and
// %d — never a unit.
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

// Reseed a controller's own text buffer from its millimetre member, unless
// that field is being edited RIGHT NOW. Decided before the item is submitted,
// using the field's own id, so an external change — or a unit switch — shows
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
// a field reading "in" the button labelled +1 moved the value by 1 mm — 0.039
// in — and the mm min/max bounds shrank the usable range by the unit factor.
//
// Magnitudes come from the unit table as {10*step, step, 0.1*step}, which for
// millimetres is exactly {10, 1, 0.1} — the behaviour that was already there.
// Bounds stay millimetres and keep stepperRow's clamp semantics (a bound only
// stops motion TOWARDS it), so call sites pass what they always passed.
inline bool lengthStepperRow(const char* id, float* mm, bool allowNegative,
                             float minMm, float maxMm, float zeroMm = 0.0f) {
    const double s = unitInfo(currentUnit()).step;
    const double mags[3] = { 10.0 * s, s, 0.1 * s };   // DISPLAY units
    bool changed = false;
    bool first = true;
    ImGui::PushID(id);
    const float h = std::max(ImGui::GetFrameHeight(), 34.0f);

    auto button = [&](const char* label) -> bool {
        if (!first) ImGui::SameLine();
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
    {
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

inline bool lengthBufferIsActive(const char* label) {
    return ImGui::GetActiveID() == ImGui::GetID(label);
}
inline void reseedLengthBufferIfIdle(const char* label, char* buf, size_t n, double mm) {
    if (!lengthBufferIsActive(label)) formatLengthDigits(buf, n, mm);
}

inline std::string fmtVec3(double xMm, double yMm, double zMm) {
    const UnitInfo& u = unitInfo(currentUnit());
    char b[96];
    std::snprintf(b, sizeof b, "(%.*f, %.*f, %.*f) %s",
                  u.decimals, toDisplay(xMm), u.decimals, toDisplay(yMm),
                  u.decimals, toDisplay(zMm), u.suffix);
    return b;
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
// written back only when the widget reports a change — so an untouched value
// never drifts, and a unit switch shows correctly on the next frame. Step and
// precision come from the unit table; there are deliberately no step/fmt
// parameters to get wrong.
inline LengthEdit lengthField(const char* label, double* mm, ImGuiInputTextFlags flags = 0) {
    const UnitInfo& u = unitInfo(currentUnit());
    char fmt[8];
    std::snprintf(fmt, sizeof fmt, "%%.%df", u.decimals);
    double disp = toDisplay(*mm);
    const bool changed = inputNumber(label, &disp, u.step, u.step * 10.0, fmt, flags);
    const bool active  = ImGui::IsItemActive();   // valid: the item just submitted IS this field
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
