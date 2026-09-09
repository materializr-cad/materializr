// Two related sketch-inference behaviours at a CORNER - a vertex where two
// straight segments meet.
//
// 1. Corner guides (Full / Max). Tangency has no single answer at a corner:
//    the chain has two tangent directions there, one per incident segment,
//    and OnLineExtension already publishes both. The directions the corner
//    itself defines are the ones that were missing -
//      • CornerBisector, norm(a + b) for unit rays a, b to the neighbours:
//        the angle directly between the two lines (the miter direction);
//      • CornerTangent, norm(b − a): the average of the two TRAVEL directions,
//        the tangent a smooth curve through the three points would carry
//        through the vertex - exactly perpendicular to the bisector.
//
// 2. The displacement budget. Every capture cap in the resolver is absolute
//    (1.5 mm, or grid-relative), but the damage a pull does is relative to the
//    segment being drawn: 1.5 mm off a 100 mm line is nothing, 1.5 mm off a
//    2 mm line is the whole line. Steve, 2026-09-03: with geometry near the
//    anchor a 2 mm line placed fine but 1 mm and 3 mm were unreachable - the
//    alternatives being no line at all (an intersection landing ON the anchor,
//    collapsing the segment) or a 19 mm jump to a farther intersection still
//    inside the 5x pair-intersection cap. An inference may now move the
//    placement at most a quarter of the extent drawn so far.

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

// Grid snap off: it quantizes the first click, which would move the anchor and
// silently change the aim every one of these tests depends on.
struct Rig {
    Sketch sketch;
    SketchSolver solver;
    SketchTool tool;
    explicit Rig(SketchTool::InferenceLevel lvl = SketchTool::InferenceLevel::Full,
                 bool grid = false, float step = 1.0f) {
        tool.setSketch(&sketch);
        tool.setSolver(&solver);
        tool.setGridStep(step);
        tool.setSnapToGridEnabled(grid);
        tool.setInferenceLevel(lvl);
        tool.setMode(SketchToolMode::Line);
    }
    void startLineAt(glm::vec2 a) {
        tool.onMouseMove(a);
        tool.onMouseDown(a);
    }
};

bool hasKind(const SketchTool& tool, InferenceGuide::Kind k) {
    for (const auto& g : tool.getActiveInferences())
        if (g.kind == k) return true;
    return false;
}

// A right-angle corner at the origin: one arm along +X, one along +Y. The
// vertex is the sketch point the line tool will anchor on.
struct Corner {
    int vertex;
};
Corner buildRightAngle(Sketch& sk) {
    const int v  = sk.addPoint(glm::vec2(0.0f, 0.0f));
    const int ax = sk.addPoint(glm::vec2(30.0f, 0.0f));
    const int ay = sk.addPoint(glm::vec2(0.0f, 30.0f));
    sk.addLine(v, ax);
    sk.addLine(v, ay);
    return {v};
}

bool aim(SketchTool& tool, glm::vec2 anchor, float deg, float len) {
    const float r = deg * static_cast<float>(M_PI) / 180.0f;
    tool.onMouseMove(anchor + glm::vec2(std::cos(r), std::sin(r)) * len);
    return true;
}

} // namespace

// ─── 1. corner guides ────────────────────────────────────────────────────────

// The interior bisector of a 90 deg corner runs at 45 deg. Aiming down it from
// the vertex fires the guide.
TEST(CornerInference, BisectorFiresAtTheInteriorAngle) {
    Rig r;
    buildRightAngle(r.sketch);
    const glm::vec2 V(0.0f, 0.0f);
    r.startLineAt(V);
    aim(r.tool, V, 45.0f, 20.0f);
    EXPECT_TRUE(hasKind(r.tool, InferenceGuide::CornerBisector));
}

// Either way along counts: the exterior direction is the same line.
TEST(CornerInference, BisectorFiresOnTheExteriorRayToo) {
    Rig r;
    buildRightAngle(r.sketch);
    const glm::vec2 V(0.0f, 0.0f);
    r.startLineAt(V);
    aim(r.tool, V, 225.0f, 20.0f);
    EXPECT_TRUE(hasKind(r.tool, InferenceGuide::CornerBisector));
}

// The corner tangent is perpendicular to the bisector - 135 deg here - and is
// the direction a smooth curve through the three points would take.
TEST(CornerInference, CornerTangentIsPerpendicularToTheBisector) {
    Rig r;
    buildRightAngle(r.sketch);
    const glm::vec2 V(0.0f, 0.0f);
    r.startLineAt(V);
    aim(r.tool, V, 135.0f, 20.0f);
    EXPECT_TRUE(hasKind(r.tool, InferenceGuide::CornerTangent));
    EXPECT_FALSE(hasKind(r.tool, InferenceGuide::CornerBisector));
}

// It is a genuine direction lock, not a wide net: 20 deg off the bisector of a
// 90 deg corner is nowhere near either guide.
TEST(CornerInference, DoesNotFireWellOffTheCornerDirections) {
    Rig r;
    buildRightAngle(r.sketch);
    const glm::vec2 V(0.0f, 0.0f);
    r.startLineAt(V);
    aim(r.tool, V, 65.0f, 20.0f);
    EXPECT_FALSE(hasKind(r.tool, InferenceGuide::CornerBisector));
    EXPECT_FALSE(hasKind(r.tool, InferenceGuide::CornerTangent));
}

// The snapped result actually lands ON the bisector, not merely near it: the
// guide is a placement, not a decoration.
TEST(CornerInference, SnapsThePlacementOntoTheBisector) {
    Rig r;
    buildRightAngle(r.sketch);
    const glm::vec2 V(0.0f, 0.0f);
    r.startLineAt(V);
    // 1.5 deg off 45, far enough out that the absolute caps are not binding.
    const float a = 46.5f * static_cast<float>(M_PI) / 180.0f;
    const glm::vec2 cursor = V + glm::vec2(std::cos(a), std::sin(a)) * 20.0f;
    r.tool.onMouseMove(cursor);
    const glm::vec2 snapped = r.tool.getCurrentPos();
    ASSERT_TRUE(hasKind(r.tool, InferenceGuide::CornerBisector));
    EXPECT_NEAR(snapped.x, snapped.y, 1e-3f);   // x == y is the 45 deg line
}

// Reduced keeps the classic set: the corner already offers two extension
// guides there, and these would crowd them.
TEST(CornerInference, SilentAtReducedAndOff) {
    for (auto lvl : {SketchTool::InferenceLevel::Reduced,
                     SketchTool::InferenceLevel::Off}) {
        Rig r(lvl);
        buildRightAngle(r.sketch);
        const glm::vec2 V(0.0f, 0.0f);
        r.startLineAt(V);
        aim(r.tool, V, 45.0f, 20.0f);
        EXPECT_FALSE(hasKind(r.tool, InferenceGuide::CornerBisector));
        EXPECT_FALSE(hasKind(r.tool, InferenceGuide::CornerTangent));
    }
    Rig m(SketchTool::InferenceLevel::Max);
    buildRightAngle(m.sketch);
    const glm::vec2 V(0.0f, 0.0f);
    m.startLineAt(V);
    aim(m.tool, V, 45.0f, 20.0f);
    EXPECT_TRUE(hasKind(m.tool, InferenceGuide::CornerBisector));
}

// A straight-through vertex (two collinear segments) has no bisector to offer,
// and its corner tangent is just the line itself - OnLineExtension's job.
TEST(CornerInference, DegenerateStraightVertexPublishesNoCornerGuide) {
    Rig r;
    const int v = r.sketch.addPoint(glm::vec2(0.0f, 0.0f));
    const int a = r.sketch.addPoint(glm::vec2(-30.0f, 0.0f));
    const int b = r.sketch.addPoint(glm::vec2(30.0f, 0.0f));
    r.sketch.addLine(v, a);
    r.sketch.addLine(v, b);
    const glm::vec2 V(0.0f, 0.0f);
    r.startLineAt(V);
    for (float deg : {0.0f, 90.0f, 45.0f, 180.0f, 270.0f}) {
        aim(r.tool, V, deg, 20.0f);
        EXPECT_FALSE(hasKind(r.tool, InferenceGuide::CornerBisector)) << deg;
        EXPECT_FALSE(hasKind(r.tool, InferenceGuide::CornerTangent)) << deg;
    }
}

// ─── 2. displacement budget ──────────────────────────────────────────────────
//
// The mechanism behind Steve's report. Two candidate guides that each fired
// well within their own (small) tolerance can still INTERSECT far away when
// they are nearly parallel - a hair-off-vertical edge crossing the anchor's
// own vertical axis guide meets it wherever the two converge, which may be
// centimetres up the line. The pair-intersection branch then wins outright,
// with only an absolute 5x cap between the cursor and that crossing, so every
// cursor position for centimetres around resolves to the SAME point: the line
// can only be drawn at that one length.

namespace {
// Anchor at the origin (a real vertex, so the first click lands exactly), plus
// a near-vertical edge that crosses x == 0 at y == `crossAt`. Drawing straight
// up from the anchor puts the cursor inside BOTH the edge's on-line band and
// the anchor's vertical-axis band the whole way, so the pair fires throughout.
void buildNearVerticalEdge(Sketch& sk, float crossAt, float halfSpan) {
    sk.addPoint(glm::vec2(0.0f, 0.0f));
    const float dx = 0.03f;
    const int a = sk.addPoint(glm::vec2( dx, crossAt - halfSpan));
    const int b = sk.addPoint(glm::vec2(-dx, crossAt + halfSpan));
    sk.addLine(a, b);
}
} // namespace

// The reported symptom: with the crossing 10 mm up, every requested length
// from ~3 mm to ~17 mm collapsed onto 10 mm. Each request must now come out
// at the length asked for.
TEST(DisplacementBudget, ANearlyParallelPairMustNotQuantizeTheLength) {
    Rig r;
    buildNearVerticalEdge(r.sketch, 10.0f, 16.0f);
    const glm::vec2 V(0.0f, 0.0f);
    r.startLineAt(V);
    for (float want : {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 8.0f}) {
        r.tool.onMouseMove(V + glm::vec2(0.0f, want));
        const glm::vec2 got = r.tool.getCurrentPos();
        EXPECT_NEAR(glm::length(got - V), want, 0.1f)
            << "asked for " << want << " mm, got " << glm::length(got - V);
    }
}

// The same pair with the crossing ON the anchor: the intersection is the
// anchor itself, so the segment collapsed to nothing and no line was drawn.
TEST(DisplacementBudget, NeverCollapsesTheSegmentOntoItsAnchor) {
    Rig r;
    buildNearVerticalEdge(r.sketch, 0.0f, 6.0f);
    const glm::vec2 V(0.0f, 0.0f);
    r.startLineAt(V);
    for (float want : {0.5f, 1.0f, 2.0f, 3.0f}) {
        r.tool.onMouseMove(V + glm::vec2(0.0f, want));
        const glm::vec2 got = r.tool.getCurrentPos();
        EXPECT_GT(glm::length(got - V), want * 0.7f)
            << "asked for " << want << " mm, segment collapsed to "
            << glm::length(got - V);
    }
}

// The budget binds and releases on the SAME geometry, purely with the size of
// what's being drawn: a crossing 10 mm up is 10 mm of theft from a 3 mm line
// and a rounding error on a 60 mm one, so the long draw still snaps to it.
TEST(DisplacementBudget, LeavesNormalSizedGeometryAlone) {
    Rig r;
    buildNearVerticalEdge(r.sketch, 10.0f, 60.0f);
    const glm::vec2 V(0.0f, 0.0f);
    r.startLineAt(V);
    // 55 mm up: budget 13.75 mm, and the crossing at (0, 50) sits 5 mm away.
    // (Same edge, re-crossing at 10; the pair nearest the cursor wins.)
    r.tool.onMouseMove(V + glm::vec2(0.0f, 12.0f));
    const glm::vec2 got = r.tool.getCurrentPos();
    EXPECT_NEAR(glm::length(got - V), 10.0f, 0.2f)
        << "at 12 mm the 10 mm crossing is well inside the budget and should "
           "still capture";
}

// ─── 3. a highlighted guide means an EXACT angle ─────────────────────────────
//
// With snap-to-grid on, the resolver used to round a directional guide's
// result onto the lattice. For an axis-aligned guide that is free - the ray
// passes through lattice points. For a DIAGONAL one it is not: rounding both
// coordinates moves the point up to half a diagonal cell off the ray, which
// near the anchor is degrees of angular error, while the guide still
// highlights because it fired on direction. Steve, 2026-09-03: "it seems like
// there is about a 5 degree window for error when those should be pretty
// exact angles they highlight at." Measured before the fix, on a 1 mm grid:
// 8.4 deg on a 3 mm leg off a 70 deg corner, 3.4 deg at 5 mm. A 90 deg corner
// hid it completely - its 45 deg bisector is lattice-commensurate.
TEST(CornerInference, BisectorIsExactWithGridSnapOn) {
    for (float cornerDeg : {90.0f, 70.0f, 60.0f, 50.0f}) {
        for (float len : {3.0f, 5.0f, 10.0f, 40.0f}) {
            Rig r(SketchTool::InferenceLevel::Full, /*grid=*/true, /*step=*/1.0f);
            const float cr = cornerDeg * static_cast<float>(M_PI) / 180.0f;
            const int v = r.sketch.addPoint(glm::vec2(0.0f, 0.0f));
            r.sketch.addLine(v, r.sketch.addPoint(glm::vec2(30.0f, 0.0f)));
            r.sketch.addLine(v, r.sketch.addPoint(
                glm::vec2(30.0f * std::cos(cr), 30.0f * std::sin(cr))));
            const glm::vec2 V(0.0f, 0.0f);
            r.startLineAt(V);
            const float want = cr * 0.5f;
            const float off  = 0.5f * static_cast<float>(M_PI) / 180.0f;
            r.tool.onMouseMove(V + glm::vec2(std::cos(want + off),
                                             std::sin(want + off)) * len);
            const glm::vec2 got = r.tool.getCurrentPos();
            ASSERT_TRUE(hasKind(r.tool, InferenceGuide::CornerBisector))
                << "corner " << cornerDeg << " len " << len;
            const float gotA = std::atan2(got.y, got.x);
            EXPECT_NEAR(gotA * 180.0f / static_cast<float>(M_PI),
                        want * 180.0f / static_cast<float>(M_PI), 0.05f)
                << "corner " << cornerDeg << " deg, leg " << len << " mm";
        }
    }
}

// The catch window is angular at every length. perp/parallel-to-prev carries
// an absolute floor, which on a short leg opens the ANGULAR window right up
// (5.7 deg on a 3 mm leg at a 1 mm grid); an angular relationship deserves an
// angular tolerance, so the corner guides deliberately drop that floor.
TEST(CornerInference, CatchWindowStaysAngularOnAShortLeg) {
    Rig r(SketchTool::InferenceLevel::Full, /*grid=*/true, /*step=*/1.0f);
    buildRightAngle(r.sketch);
    const glm::vec2 V(0.0f, 0.0f);
    r.startLineAt(V);
    // 8 deg off the 45 deg bisector on a 3 mm leg: well outside 3 deg, and
    // must not highlight however short the segment is.
    aim(r.tool, V, 53.0f, 3.0f);
    EXPECT_FALSE(hasKind(r.tool, InferenceGuide::CornerBisector));
    // 2 deg off still catches.
    aim(r.tool, V, 47.0f, 3.0f);
    EXPECT_TRUE(hasKind(r.tool, InferenceGuide::CornerBisector));
}
