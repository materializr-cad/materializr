#pragma once
// Text anchored to world geometry (dimension labels, live lengths, hints) goes
// through here instead of ImDrawList::AddText directly.
//
// At high zoom a point only a few hundred millimetres away projects to millions
// of screen pixels. ImGui clips glyph quads on the CPU by interpolating their
// UVs, and at those magnitudes the float arithmetic collapses: the quad comes
// out with a UV span covering many glyphs, and the screen fills with enormous
// garbled characters from the font atlas (issue #172, shot 5). Text that far
// off screen is invisible anyway, so it is simply not drawn.

#include <imgui.h>
#include <cmath>

namespace materializr {

// Beyond this magnitude (px) a label cannot be on any screen we run on, and the
// glyph clip arithmetic is no longer trustworthy.
constexpr float kMaxTextCoord = 20000.0f;

inline bool textPosDrawable(const ImVec2& p) {
    return std::fabs(p.x) < kMaxTextCoord && std::fabs(p.y) < kMaxTextCoord;   // false for NaN too
}

inline void drawTextSafe(ImDrawList* dl, const ImVec2& pos, ImU32 col,
                         const char* text, const char* textEnd = nullptr) {
    if (!dl || !textPosDrawable(pos)) return;
    dl->AddText(pos, col, text, textEnd);
}

} // namespace materializr
