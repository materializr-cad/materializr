// Regression: the tangent-to-circle/arc inference must fire at the FULL
// inference level, not only when the line's anchor stands clear of the curve.
//
// The tangent guide computed its two tangent rays from the anchor with
// asin(R/D) and then rejected anything with `D <= R`, so it only ever fired
// for an anchor outside the circle. That threw away the two cases people
// actually mean by "tangent":
//
//   * start a line at a point ON a circle and run it off tangentially;
//   * carry a line on tangentially from the END of an arc.
//
// In both, D == R exactly (the on-rim snap puts the anchor on the curve), the
// formula degenerates cleanly onto the single tangent there - asin(1) = 90°
// off the radius, in both directions - and the guard rejected it one epsilon
// early. Nothing fired; 15° angle-snap silently took the placement instead.
//
// Two narrower faults fixed alongside, covered below:
//   * an ARC was treated as its whole circle, so a guide could point at a
//     tangency on the phantom part the arc does not cover;
//   * first-match-wins picked whichever curve came first in the sketch's list
//     rather than the one being aimed at.

#include "modeling/Sketch.h"
#include "modeling/SketchTool.h"
#include "modeling/SketchSolver.h"
#include "modeling/SvgImport.h"
#include "modeling/TextSketchOp.h"

#include <gtest/gtest.h>
#include <glm/glm.hpp>
#include <cmath>
#include <string>

// SketchTool's Text / SVG stamp paths are not part of materializr_core (they
// pull in font rendering); stub them so this links.
namespace materializr {
int SvgImport::place(Sketch*, const SvgPaths&, glm::vec2, float, float) { return 0; }
int TextSketch::generate(Sketch*, const std::string&, const std::string&,
                         glm::vec2, float, float) { return 0; }
}

using materializr::InferenceGuide;
using materializr::Sketch;
using materializr::SketchSolver;
using materializr::SketchTool;
using materializr::SketchToolMode;

namespace {

constexpr float kR = 10.0f;

// A sketch + tool wired for line placement at a given inference level. Grid
// snap is off by default: it quantizes the FIRST CLICK, which would move the
// anchor and silently change the aim these tests depend on.
struct Rig {
    Sketch sketch;
    SketchSolver solver;
    SketchTool tool;
    explicit Rig(SketchTool::InferenceLevel lvl = SketchTool::InferenceLevel::Full,
                 bool grid = false) {
        tool.setSketch(&sketch);
        tool.setSolver(&solver);
        tool.setGridStep(1.0f);
        tool.setSnapToGridEnabled(grid);
        tool.setInferenceLevel(lvl);
        tool.setMode(SketchToolMode::Line);
    }
    void startLineAt(glm::vec2 a) {
        tool.onMouseMove(a);
        tool.onMouseDown(a);
    }
};

bool hasTangent(const SketchTool& tool) {
    for (const auto& g : tool.getActiveInferences())
        if (g.kind == InferenceGuide::TangentToCircle) return true;
    return false;
}

int tangentRef(const SketchTool& tool) {
    for (const auto& g : tool.getActiveInferences())
        if (g.kind == InferenceGuide::TangentToCircle) return g.refId;
    return -1;
}

// Aim the cursor `len` from `anchor` along `angleRad` and report whether a
// tangent guide fired.
bool aim(SketchTool& tool, glm::vec2 anchor, float angleRad, float len) {
    tool.onMouseMove(anchor +
                     glm::vec2(std::cos(angleRad), std::sin(angleRad)) * len);
    return hasTangent(tool);
}

} // namespace

// The case that already worked, kept so a future tightening can't quietly
// take it away: anchor well clear of the circle, aimed down a tangent ray.
TEST(TangentInference, FiresFromAnAnchorOutsideTheCircle) {
    Rig r;
    const int c = r.sketch.addPoint(glm::vec2(0.0f, 0.0f));
    r.sketch.addCircle(c, kR);
    const glm::vec2 A(-40.0f, 0.0f);
    r.startLineAt(A);
    const float tAng = std::asin(kR / 40.0f);   // ray angle off the +X axis
    EXPECT_TRUE(aim(r.tool, A, tAng, 15.0f));
    EXPECT_TRUE(aim(r.tool, A, -tAng, 15.0f));  // the mirrored tangent
}

// The bug. Anchor sits ON the rim - where clicking a circle actually puts it -
// and the tangent there is perpendicular to the radius, in both directions.
TEST(TangentInference, FiresFromAnAnchorOnTheRim) {
    Rig r;
    const int c = r.sketch.addPoint(glm::vec2(0.0f, 0.0f));
    r.sketch.addCircle(c, kR);
    const glm::vec2 A(kR, 0.0f);                // rim point on +X
    r.startLineAt(A);
    const float up = static_cast<float>(M_PI) * 0.5f;
    EXPECT_TRUE(aim(r.tool, A, up, 15.0f));
    EXPECT_TRUE(aim(r.tool, A, -up, 15.0f));
    // And it is a genuine direction lock, not a coincidence: 20° off the
    // tangent must NOT report one.
    EXPECT_FALSE(aim(r.tool, A, up - static_cast<float>(M_PI) / 9.0f, 15.0f));
}

// Same shape, from the end of an arc - carrying a profile on tangentially is
// the common one (slot ends, fillet run-outs).
TEST(TangentInference, FiresFromAnArcEndpoint) {
    Rig r;
    const int c = r.sketch.addPoint(glm::vec2(0.0f, 0.0f));
    const int s = r.sketch.addPoint(glm::vec2(kR, 0.0f));
    const int e = r.sketch.addPoint(glm::vec2(0.0f, kR));
    r.sketch.addArc(c, s, e, kR);               // CCW quarter, 0° -> 90°
    const glm::vec2 A(0.0f, kR);                // the arc's end, on +Y
    r.startLineAt(A);
    const float tAng = static_cast<float>(M_PI);  // tangent there runs along -X
    EXPECT_TRUE(aim(r.tool, A, tAng, 15.0f));
}

// An arc is not its circle: a tangency landing on the missing three quarters
// points at geometry that isn't there.
TEST(TangentInference, IgnoresATangencyPointOffTheArc) {
    Rig r;
    const int c = r.sketch.addPoint(glm::vec2(0.0f, 0.0f));
    const int s = r.sketch.addPoint(glm::vec2(kR, 0.0f));
    const int e = r.sketch.addPoint(glm::vec2(0.0f, kR));
    r.sketch.addArc(c, s, e, kR);               // CCW quarter, 0° -> 90°
    const glm::vec2 A(0.0f, -40.0f);            // below the centre
    r.startLineAt(A);
    // Both tangents from here touch near the BOTTOM of the circle, outside the
    // 0°–90° span.
    const float base = static_cast<float>(M_PI) * 0.5f;   // A -> centre is +Y
    const float off  = std::asin(kR / 40.0f);
    EXPECT_FALSE(aim(r.tool, A, base + off, 30.0f));
    EXPECT_FALSE(aim(r.tool, A, base - off, 30.0f));
    // The same circle WITHOUT the arc restriction does fire, proving the miss
    // above is the span check and not a broken aim.
    Rig full;
    const int fc = full.sketch.addPoint(glm::vec2(0.0f, 0.0f));
    full.sketch.addCircle(fc, kR);
    full.startLineAt(A);
    EXPECT_TRUE(aim(full.tool, A, base + off, 30.0f));
}

// With two circles in range the guide must name the one being aimed at, not
// whichever the sketch happens to list first.
TEST(TangentInference, PicksTheCurveBeingAimedAt) {
    Rig r;
    const int c1 = r.sketch.addPoint(glm::vec2(0.0f, 0.0f));
    const int id1 = r.sketch.addCircle(c1, kR);          // listed first
    const int c2 = r.sketch.addPoint(glm::vec2(0.0f, 30.0f));
    const int id2 = r.sketch.addCircle(c2, kR);
    const glm::vec2 A(-40.0f, 0.0f);
    r.startLineAt(A);

    const glm::vec2 toC2 = glm::vec2(0.0f, 30.0f) - A;
    const float tAng2 = std::atan2(toC2.y, toC2.x) +
                        std::asin(kR / glm::length(toC2));
    ASSERT_TRUE(aim(r.tool, A, tAng2, 30.0f));
    EXPECT_EQ(tangentRef(r.tool), id2);

    const float tAng1 = std::asin(kR / 40.0f);
    ASSERT_TRUE(aim(r.tool, A, tAng1, 30.0f));
    EXPECT_EQ(tangentRef(r.tool), id1);
}

// An anchor strictly inside a circle has no tangent through it at all - the
// guard that case needs must survive, and must not produce a NaN direction.
TEST(TangentInference, NeverFiresFromInsideTheCircle) {
    Rig r;
    const int c = r.sketch.addPoint(glm::vec2(0.0f, 0.0f));
    r.sketch.addCircle(c, kR);
    const glm::vec2 A(2.0f, 0.0f);              // well inside
    r.startLineAt(A);
    for (int deg = 0; deg < 360; deg += 5) {
        const float a = static_cast<float>(deg) * static_cast<float>(M_PI) / 180.0f;
        EXPECT_FALSE(aim(r.tool, A, a, 15.0f)) << "fired at " << deg << " deg";
    }
}

// The whole point of the report: this is FULL, not just MAX. Full must catch a
// tangent aimed dead-on.
TEST(TangentInference, FullIsEnoughNotJustMax) {
    for (auto lvl : {SketchTool::InferenceLevel::Full,
                     SketchTool::InferenceLevel::Max}) {
        Rig r(lvl);
        const int c = r.sketch.addPoint(glm::vec2(0.0f, 0.0f));
        r.sketch.addCircle(c, kR);
        const glm::vec2 A(kR, 0.0f);
        r.startLineAt(A);
        EXPECT_TRUE(aim(r.tool, A, static_cast<float>(M_PI) * 0.5f, 15.0f))
            << "level " << static_cast<int>(lvl);
    }
    // ...and Off still means off.
    Rig off(SketchTool::InferenceLevel::Off);
    const int c = off.sketch.addPoint(glm::vec2(0.0f, 0.0f));
    off.sketch.addCircle(c, kR);
    const glm::vec2 A(kR, 0.0f);
    off.startLineAt(A);
    EXPECT_FALSE(aim(off.tool, A, static_cast<float>(M_PI) * 0.5f, 15.0f));
}
