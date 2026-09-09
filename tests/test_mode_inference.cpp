// Regression: the directional inference family must work in every draw tool
// that asks the user to pick a DIRECTION, not just the Line tool.
//
// Perpendicular/parallel-to-previous, tangent-to-curve, angle snap and the
// hover-charged guides were all written for Line and gated on
// `m_mode == SketchToolMode::Line`. Arcs, circles, polygons and splines
// therefore drew with no directional assistance at all - no 15° angle snap on
// an arc's chord, no tangent guide while starting a spline, nothing. The gate
// is now SketchTool::directionalAnchor(), which answers both "does a direction
// apply here" and "measured from where".
//
// The "from where" half matters on its own: m_firstClick chains on the Line
// tool but stays on control point #1 for the whole life of a spline, so
// anchoring the guides on it would have measured every spline segment from the
// wrong end.
//
// Two modes are deliberately left inert and are pinned so below:
//   * Rectangle - axis-aligned by construction, there is no direction to pick;
//   * Circle in centre-radius mode - only the DISTANCE means anything, and
//     steering the direction would just perturb the radius.

#include "modeling/Sketch.h"
#include "modeling/SketchTool.h"
#include "modeling/SketchSolver.h"
#include "modeling/SvgImport.h"
#include "modeling/TextSketchOp.h"

#include <gtest/gtest.h>
#include <glm/glm.hpp>
#include <cmath>
#include <string>
#include <vector>

// SketchTool's Text / SVG stamp paths are not part of materializr_core (they
// pull in font rendering); stub them so this links.
namespace materializr {
int SvgImport::place(Sketch*, const SvgPaths&, glm::vec2, float, float) { return 0; }
int TextSketch::generate(Sketch*, const std::string&, const std::string&,
                         glm::vec2, float, float) { return 0; }
}

using materializr::InferenceGuide;
using materializr::Sketch;
using materializr::SketchPoint;
using materializr::SketchSolver;
using materializr::SketchTool;
using materializr::SketchToolMode;

namespace {

constexpr float kR = 10.0f;
constexpr float kDeg = static_cast<float>(M_PI) / 180.0f;

struct Rig {
    Sketch sketch;
    SketchSolver solver;
    SketchTool tool;
    explicit Rig(SketchToolMode m) {
        tool.setSketch(&sketch);
        tool.setSolver(&solver);
        tool.setGridStep(1.0f);
        // Grid snap OFF: it quantizes the first click, which would move the
        // anchor these tests aim from.
        tool.setSnapToGridEnabled(false);
        tool.setInferenceLevel(SketchTool::InferenceLevel::Full);
        tool.setAngleSnapDeg(15);
        tool.setMode(m);
    }
    // A circle at the origin to be tangent to.
    int addCircle() {
        const int c = sketch.addPoint(glm::vec2(0.0f, 0.0f));
        return sketch.addCircle(c, kR);
    }
    void click(glm::vec2 p) { tool.onMouseMove(p); tool.onMouseDown(p); }
    void aim(glm::vec2 anchor, float deg, float len) {
        tool.onMouseMove(anchor + glm::vec2(std::cos(deg * kDeg),
                                            std::sin(deg * kDeg)) * len);
    }
};

bool has(const SketchTool& t, InferenceGuide::Kind k) {
    for (const auto& g : t.getActiveInferences())
        if (g.kind == k) return true;
    return false;
}

// The anchor every test aims from, standing clear of the circle. Its tangent
// rays leave at ±asin(10/40) = ±14.478°.
const glm::vec2 kAnchor(-40.0f, 0.0f);
const float kTanDeg = std::asin(kR / 40.0f) / kDeg;

} // namespace

TEST(ModeInference, ArcChordGetsAngleSnapAndTangent) {
    { Rig r(SketchToolMode::Arc); r.addCircle(); r.click(kAnchor);
      r.aim(kAnchor, 30.0f, 20.0f);
      EXPECT_TRUE(has(r.tool, InferenceGuide::AngleSnap)); }
    { Rig r(SketchToolMode::Arc); r.addCircle(); r.click(kAnchor);
      r.aim(kAnchor, kTanDeg, 20.0f);
      EXPECT_TRUE(has(r.tool, InferenceGuide::TangentToCircle)); }
}

// Click 3 sweeps the apex and has its own 15° SWEEP snap (arcApexSnap); a
// direction guide from the chord start would pull against it.
TEST(ModeInference, ArcApexStaysInert) {
    Rig r(SketchToolMode::Arc);
    r.addCircle();
    r.click(kAnchor);
    r.click(glm::vec2(-10.0f, 0.0f));        // chord end -> now sweeping
    ASSERT_EQ(r.tool.getClickCount(), 2);
    glm::vec2 unused;
    EXPECT_FALSE(r.tool.directionalAnchor(unused));
}

// The drag sets the circumradius AND the polygon's rotation, so the direction
// is real geometry.
TEST(ModeInference, PolygonVertexGetsAngleSnap) {
    Rig r(SketchToolMode::Polygon);
    r.click(kAnchor);
    r.aim(kAnchor, 30.0f, 20.0f);
    EXPECT_TRUE(has(r.tool, InferenceGuide::AngleSnap));
}

TEST(ModeInference, CircleTwoPointGetsGuidesAndKeepsItsDiameter) {
    Rig r(SketchToolMode::Circle);
    r.tool.setCircleMode(SketchTool::CircleMode::TwoPoint);
    r.click(kAnchor);
    r.aim(kAnchor, 30.5f, 20.0f);            // half a degree off a 15° step
    EXPECT_TRUE(has(r.tool, InferenceGuide::AngleSnap));
    // Snapping the DIRECTION must not resize the circle.
    EXPECT_NEAR(glm::length(r.tool.getCurrentPos() - kAnchor), 20.0f, 1e-3f);
}

// Centre-radius: only the distance means anything.
TEST(ModeInference, CircleCentreRadiusStaysInert) {
    Rig r(SketchToolMode::Circle);
    r.tool.setCircleMode(SketchTool::CircleMode::Center);
    r.click(kAnchor);
    glm::vec2 unused;
    EXPECT_FALSE(r.tool.directionalAnchor(unused));
    r.aim(kAnchor, 30.0f, 20.0f);
    EXPECT_FALSE(has(r.tool, InferenceGuide::AngleSnap));
}

// Axis-aligned by construction - the "segment" is the diagonal, and steering it
// would only distort the box.
TEST(ModeInference, RectangleStaysInert) {
    Rig r(SketchToolMode::Rectangle);
    r.click(glm::vec2(0.0f, 0.0f));
    glm::vec2 unused;
    EXPECT_FALSE(r.tool.directionalAnchor(unused));
}

TEST(ModeInference, SplineControlPointGetsAngleSnapAndTangent) {
    { Rig r(SketchToolMode::Spline); r.addCircle();
      r.click(glm::vec2(-60.0f, 0.0f));
      r.click(kAnchor);
      r.aim(kAnchor, 30.0f, 20.0f);
      EXPECT_TRUE(has(r.tool, InferenceGuide::AngleSnap)); }
    { Rig r(SketchToolMode::Spline); r.addCircle();
      r.click(glm::vec2(-60.0f, 0.0f));
      r.click(kAnchor);
      r.aim(kAnchor, kTanDeg, 20.0f);
      EXPECT_TRUE(has(r.tool, InferenceGuide::TangentToCircle)); }
}

// A spline's control polygon is what the user steers, so its last leg is the
// "previous direction" for the perpendicular/parallel guides.
TEST(ModeInference, SplineGetsPerpendicularToTheLastLeg) {
    Rig r(SketchToolMode::Spline);
    r.click(glm::vec2(-60.0f, 0.0f));
    r.click(kAnchor);                        // last leg runs +X
    r.aim(kAnchor, 90.0f, 20.0f);
    EXPECT_TRUE(has(r.tool, InferenceGuide::PerpToPrev));
}

// The anchor trap: m_firstClick never leaves control point #1, so the guides
// have to measure from the point being extended instead. Aim 45° from the
// SECOND control point at a bearing that is not a 15° multiple from the first,
// and check the result really is 45° from the second.
TEST(ModeInference, SplineGuidesMeasureFromTheLastControlPoint) {
    Rig r(SketchToolMode::Spline);
    const glm::vec2 p0(0.0f, 0.0f), p1(20.0f, 0.0f);
    r.click(p0);
    r.click(p1);

    glm::vec2 anchor;
    ASSERT_TRUE(r.tool.directionalAnchor(anchor));
    EXPECT_NEAR(anchor.x, p1.x, 1e-4f);
    EXPECT_NEAR(anchor.y, p1.y, 1e-4f);
    EXPECT_GT(glm::length(anchor - p0), 1.0f) << "anchored on the FIRST point";

    r.aim(p1, 45.4f, 20.0f);                 // inside the snap window of 45°
    ASSERT_TRUE(has(r.tool, InferenceGuide::AngleSnap));
    const glm::vec2 got = r.tool.getCurrentPos();
    const float fromLast = std::atan2(got.y - p1.y, got.x - p1.x) / kDeg;
    EXPECT_NEAR(fromLast, 45.0f, 0.05f);
    // Measured from the first control point that same point is nowhere near a
    // 15° step, which is what makes this test discriminating.
    const float fromFirst = std::atan2(got.y - p0.y, got.x - p0.x) / kDeg;
    EXPECT_GT(std::abs(fromFirst - std::round(fromFirst / 15.0f) * 15.0f), 1.0f);
}

// Splines were invisible to the tangent guide - it only ever scanned circles
// and arcs. Control points laid around a circle of radius 10 give a known
// answer: the tangent from (-40,0) leaves at ±14.478°, same as the real circle.
TEST(ModeInference, TangentToASpline) {
    Rig r(SketchToolMode::Line);
    std::vector<int> cps;
    for (int i = 0; i <= 12; ++i) {
        const float a = static_cast<float>(i) / 12.0f * 2.0f *
                        static_cast<float>(M_PI);
        cps.push_back(r.sketch.addPoint(
            glm::vec2(kR * std::cos(a), kR * std::sin(a))));
    }
    r.sketch.addSpline(cps);
    r.click(kAnchor);
    r.aim(kAnchor, kTanDeg, 30.0f);
    EXPECT_TRUE(has(r.tool, InferenceGuide::TangentToCircle));
    // A direction that is not tangent must stay quiet.
    r.aim(kAnchor, 60.0f, 30.0f);
    EXPECT_FALSE(has(r.tool, InferenceGuide::TangentToCircle));
}

// The spline tangent is found on a sampled polyline, so its accuracy is a
// deliberate trade against how much sampling every mouse-move pays for. Pin it:
// the guide direction must land far inside the 3-degree catch window, or the
// sampling density has been cut too far and the guide is pointing somewhere the
// curve is not.
TEST(ModeInference, SplineTangentIsAccurate) {
    Rig r(SketchToolMode::Line);
    r.tool.setAngleSnapDeg(0);          // keep the angle-snap guide out of it
    std::vector<int> cps;
    for (int i = 0; i <= 12; ++i) {
        const float a = static_cast<float>(i) / 12.0f * 2.0f *
                        static_cast<float>(M_PI);
        cps.push_back(r.sketch.addPoint(
            glm::vec2(kR * std::cos(a), kR * std::sin(a))));
    }
    r.sketch.addSpline(cps);
    r.click(kAnchor);

    // Walk the catch window and take its centre - that is where the engine
    // believes the tangent lies.
    float lo = 1e9f, hi = -1e9f;
    for (int i = -400; i <= 400; ++i) {
        const float deg = kTanDeg + static_cast<float>(i) * 0.01f;
        r.aim(kAnchor, deg, 30.0f);
        if (has(r.tool, InferenceGuide::TangentToCircle)) {
            lo = std::min(lo, deg);
            hi = std::max(hi, deg);
        }
    }
    ASSERT_LT(lo, hi) << "no tangent fired anywhere near the true ray";
    EXPECT_NEAR(0.5f * (lo + hi), kTanDeg, 0.5f);
}

// A tangency that falls OUTSIDE the drawn span is not a tangent to this curve.
// The arch y = 10 - 0.025x² over x ∈ [-10, 10] has its two tangency points from
// (-40,-20) at x = -20 and x = -60, both off the curve.
TEST(ModeInference, SplineTangencyOffTheDrawnSpanIsIgnored) {
    Rig r(SketchToolMode::Line);
    std::vector<int> cps;
    for (int i = 0; i <= 4; ++i) {
        const float x = -10.0f + 5.0f * static_cast<float>(i);
        cps.push_back(r.sketch.addPoint(glm::vec2(x, 10.0f - 0.025f * x * x)));
    }
    r.sketch.addSpline(cps);
    const glm::vec2 P(-40.0f, -20.0f);
    r.click(P);
    for (int i = 0; i <= 900; ++i) {
        r.aim(P, static_cast<float>(i) * 0.1f, 40.0f);
        ASSERT_FALSE(has(r.tool, InferenceGuide::TangentToCircle))
            << "fired at " << static_cast<float>(i) * 0.1f << " deg";
    }
}
