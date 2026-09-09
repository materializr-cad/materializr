#pragma once
#include <glm/glm.hpp>
#include <string>
#include <vector>

namespace materializr {

class Sketch;

// SVG file → sketch geometry (parsed by the vendored nanosvg). Every path -
// shapes, transforms, groups, viewBox included - arrives flattened to cubic
// béziers, which load() samples into fine polylines in the SVG's own
// coordinate space (Y-down). place() then scales / rotates / Y-flips them
// into the sketch as closed line loops, so an imported logo behaves exactly
// like drawn geometry: regions form, extrude works, Project Sketch wraps it
// onto curved faces.
struct SvgPaths {
    std::vector<std::vector<glm::vec2>> loops; // sampled, raw SVG units
    std::vector<bool> closed;                  // per loop
    glm::vec2 bbMin{0.0f}, bbMax{0.0f};
    bool empty() const { return loops.empty(); }
    glm::vec2 size() const { return bbMax - bbMin; }
};

class SvgImport {
public:
    // Parse + sample. False when the file doesn't parse or holds no paths.
    static bool load(const std::string& path, SvgPaths& out);

    // Insert into the sketch at `pos` (anchor = bounding-box centre),
    // scaled so the artwork's width is `widthMm`, rotated `angleDeg` CCW.
    // Geometry is tagged fromText so it stays out of vertex markers and
    // snap/inference, same as Text-tool glyphs. Returns loops added.
    static int place(Sketch* sketch, const SvgPaths& svg, glm::vec2 pos,
                     float widthMm, float angleDeg);
};

} // namespace materializr
