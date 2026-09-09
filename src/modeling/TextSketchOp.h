#pragma once
#include <glm/glm.hpp>
#include <string>
#include <vector>

namespace materializr {

class Sketch;

// TrueType text → sketch geometry. Renders glyphs with OCCT's
// Font_BRepFont (real outlines, holes in O/A/B included, kerning included),
// samples each outline wire to a fine polyline and adds it to the sketch as
// closed line loops - so the letters form proper regions: extrudable,
// push/pullable, and projectable onto curved faces for engraving.
//
// (This file used to hold a hand-drawn single-stroke font that was never
// wired to the UI; open strokes can't form regions, so it could never have
// been engraved. Replaced wholesale.)
class TextSketch {
public:
    // Insert `text` at sketch-space `pos` (baseline-left anchor), rotated
    // `angleDeg` CCW about the anchor. `heightMm` is the CAPITAL letter
    // height (what calipers would measure on an 'H'), not the typographic
    // em size. Returns the number of closed loops added; 0 means nothing
    // was inserted (missing font, empty text, degenerate height).
    static int generate(Sketch* sketch, const std::string& text,
                        const std::string& fontPath, glm::vec2 pos,
                        float heightMm, float angleDeg);

    // Text extents relative to the baseline-left anchor, UNROTATED, in mm:
    // bbMin.y is the descender (negative for g/j/y), bbMax.y the ascender.
    // Used for the placement preview rectangle. False when the font is
    // missing or the text renders to nothing.
    static bool measure(const std::string& text, const std::string& fontPath,
                        float heightMm, glm::vec2& bbMin, glm::vec2& bbMax);

    // Glyph contours relative to the baseline-left anchor, UNROTATED, in mm -
    // the SAME coordinate space as measure()'s bbox. Each entry is one closed
    // contour (polyline). Drives a live preview of the actual letters before
    // placement. False when the font is missing / text renders to nothing.
    static bool outline(const std::string& text, const std::string& fontPath,
                        float heightMm,
                        std::vector<std::vector<glm::vec2>>& loops);
};

} // namespace materializr
