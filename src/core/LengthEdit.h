#pragma once
// The unit-conversion TRANSACTIONS behind every length widget, with no ImGui in
// sight so they can be tested headless. ui/LengthField.h is a thin skin over
// these: it submits the ImGui item and hands the result here.
//
// Every function takes and returns MILLIMETRES on the model side. The display
// side is a transient value that exists for one frame or one commit — it is
// never stored, which is what stops mm -> display -> mm drift from accumulating
// on the float members most Ops use.

#include "Units.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace materializr {

// A numeric field reported a change: the value the user now sees, in display
// units, becomes the model value in mm. This is the ONLY write-back path for
// lengthField and amountLengthField; test it, mutate it, and the widgets are
// covered.
inline double lengthFieldCommit(double displayValue) { return toMm(displayValue); }

// A slider must convert its VALUE and its BOUNDS together, or a converted value
// slides against mm bounds and the usable range is off by the unit factor —
// 304.8x under feet. Quantise the display value to the unit's own step so a
// drag lands on round numbers in the unit the user is looking at.
struct SliderShadow { double value, lo, hi; };
inline SliderShadow sliderShadow(double mm, double minMm, double maxMm) {
    return { toDisplay(mm), toDisplay(minMm), toDisplay(maxMm) };
}

// Drag handles snap. The old code snapped to 0.1 MM (`round(v*10)/10`) whatever
// the display unit, which under inches is a non-round 0.0039 in grid. Snap in
// the display unit's step instead, then return mm.
inline double quantiseDragMm(double mm) {
    const double step = unitInfo(currentUnit()).dragStep;
    return toMm(std::round(toDisplay(mm) / step) * step);
}

// Format a mm value into a text buffer in display units, digits only (no
// suffix — the field's label carries it). Returns false when the buffer is
// too small; the buffer is then left untouched.
inline bool formatLengthDigits(char* buf, size_t n, double mm) {
    const int written = std::snprintf(buf, n, "%.*f", unitInfo(currentUnit()).decimals, toDisplay(mm));
    return written >= 0 && static_cast<size_t>(written) < n;
}

// Reseed a text buffer from the model — but ONLY when that field is not being
// edited. The caller decides `active` BEFORE submitting the item (ImGui's
// GetActiveID() against the field's own id), so an external change or a unit
// switch shows correctly this frame while a half-typed value is never
// clobbered. Returns whether the buffer was rewritten.
inline bool reseedBuffer(char* buf, size_t n, double mm, bool active) {
    if (active) return false;
    return formatLengthDigits(buf, n, mm);
}

// The three shapes a sketch dimension takes. Angles are NOT lengths and never
// touch the unit; a circle's Radius constraint is typed and shown as its
// DIAMETER (matching the Ø label) while an arc's is shown as a radius.
enum class DimKind { Length, Radius, Angle };

// Seed the edit buffer for a constraint: what the user should see and edit.
inline bool seedDimensionText(char* buf, size_t n, DimKind kind, bool isArc, double value) {
    switch (kind) {
    case DimKind::Angle: {
        const int w = std::snprintf(buf, n, "%.2f", value * 180.0 / M_PI);
        return w >= 0 && static_cast<size_t>(w) < n;
    }
    case DimKind::Radius: return formatLengthDigits(buf, n, isArc ? value : value * 2.0);
    case DimKind::Length: return formatLengthDigits(buf, n, value);
    }
    return false;
}

// Commit a typed dimension. Angle: degrees -> radians, no unit involved.
// Otherwise: typed text -> mm FIRST (suffix honoured, else current unit), and
// only THEN is a circle's diameter halved to the stored radius. Convert, then
// halve — never the reverse, never twice. Returns false (value untouched) on
// unparseable input or a non-positive length.
inline bool applyDimensionEdit(DimKind kind, bool isArc, const char* buf, double& value) {
    if (kind == DimKind::Angle) {
        // Strict: the whole string must be one finite number. Bare strtod would
        // read the 90 out of "90in" and silently ignore the rest.
        if (!buf) return false;
        char* end = nullptr;
        const double deg = std::strtod(buf, &end);
        if (end == buf || !std::isfinite(deg)) return false;
        while (*end == ' ' || *end == '\t') ++end;
        if (*end != '\0') return false;
        value = deg * M_PI / 180.0;
        return true;
    }
    double mm = 0.0;
    if (!parseLength(buf, mm) || mm <= 0.0) return false;
    value = (kind == DimKind::Radius && !isArc) ? mm * 0.5 : mm;
    return true;
}

} // namespace materializr
