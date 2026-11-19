// #183: an arc was hit-tested as the whole circle it was cut from, so its
// selection zone ran past the ends and - for an arc left by Trim - along the
// full original circle. Picking now measures the distance to the arc itself.

#include <gtest/gtest.h>

#include "modeling/Sketch.h"
#include "modeling/SketchSolver.h"
#include "modeling/SketchTool.h"
#include "modeling/SvgImport.h"
#include "modeling/TextSketchOp.h"

#include <glm/glm.hpp>
#include <cmath>

namespace materializr {
int SvgImport::place(Sketch*, const SvgPaths&, glm::vec2, float, float) { return 0; }
int TextSketch::generate(Sketch*, const std::string&, const std::string&,
                         glm::vec2, float, float) { return 0; }
}

using materializr::Sketch;
using materializr::SketchSolver;
using materializr::SketchTool;
using materializr::SketchToolMode;

namespace {
const float kPi = 3.14159265f;

glm::vec2 onCircle(float deg, float r = 10.0f) {
    return {r * std::cos(deg * kPi / 180.0f), r * std::sin(deg * kPi / 180.0f)};
}

// A radius-10 arc about the origin from 0 to 90 degrees (counter-clockwise).
struct QuarterArc {
    Sketch sk;
    int arcId = -1;
    QuarterArc() {
        int c = sk.addPoint({0, 0});
        int s = sk.addPoint(onCircle(0));
        int e = sk.addPoint(onCircle(90));
        arcId = sk.addArc(c, s, e, 10.0);
    }
    float dist(glm::vec2 p) const { return sk.distanceToArc(sk.getArcs().front(), p); }
};

struct Rig {
    SketchSolver solver;
    SketchTool tool;
    explicit Rig(Sketch& sk) {
        tool.setSketch(&sk);
        tool.setSolver(&solver);
        tool.setGridStep(1.0f);
        tool.setMode(SketchToolMode::Select);
    }
    bool picksArc(glm::vec2 p, int arcId) {
        tool.onMouseDown(p);
        tool.onMouseUp(p);
        return tool.getSelectedArcs().count(arcId) > 0;
    }
};
}

TEST(ArcPickZone, DistanceIsToTheRimInsideTheSweep) {
    QuarterArc a;
    EXPECT_NEAR(a.dist(onCircle(45, 10)), 0.0f, 1e-4f);
    EXPECT_NEAR(a.dist(onCircle(45, 12)), 2.0f, 1e-4f);
    EXPECT_NEAR(a.dist(onCircle(45, 7)), 3.0f, 1e-4f);
}

TEST(ArcPickZone, DistanceBeyondTheEndsIsToTheNearerEndPoint) {
    QuarterArc a;
    // 180 degrees round the circle is nowhere near the arc: the old full-circle
    // distance said 0 here.
    EXPECT_GT(a.dist(onCircle(180)), 10.0f);
    // Just past the 90 degree end: distance to that end, not to the rim.
    const glm::vec2 p = onCircle(90 + 5);
    EXPECT_NEAR(a.dist(p), glm::distance(p, onCircle(90)), 1e-4f);
}

TEST(ArcPickZone, SweepThatCrossesZeroDegreesIsHandled) {
    Sketch sk;
    int c = sk.addPoint({0, 0});
    int s = sk.addPoint(onCircle(300));
    int e = sk.addPoint(onCircle(60));               // 300 -> 60 crosses 0
    sk.addArc(c, s, e, 10.0);
    const auto& arc = sk.getArcs().front();
    EXPECT_NEAR(sk.distanceToArc(arc, onCircle(0)), 0.0f, 1e-4f);
    EXPECT_NEAR(sk.distanceToArc(arc, onCircle(330)), 0.0f, 1e-4f);
    EXPECT_GT(sk.distanceToArc(arc, onCircle(180)), 10.0f);
}

TEST(ArcPickZone, SelectPicksTheDrawnArcButNotTheRestOfItsCircle) {
    QuarterArc a;
    Rig r(a.sk);
    EXPECT_FALSE(r.picksArc(onCircle(200), a.arcId)) << "the far side of the circle";
    EXPECT_FALSE(r.picksArc(onCircle(-45), a.arcId)) << "below the arc's start";
    EXPECT_TRUE(r.picksArc(onCircle(45), a.arcId)) << "on the arc";
}

TEST(ArcPickZone, ArcLeftByTrimIsOnlyPickableWhereItIs) {
    Sketch sk;
    int c = sk.addPoint({0, 0});
    sk.addCircle(c, 10.0);
    int a = sk.addPoint({5, -20}), b = sk.addPoint({5, 20});
    sk.addLine(a, b);                                // chord at x = 5 crosses the circle twice
    SketchSolver solver;
    SketchTool tool;
    tool.setSketch(&sk);
    tool.setSolver(&solver);
    tool.setGridStep(1.0f);
    tool.setMode(SketchToolMode::Trim);
    tool.onMouseDown({10.0f, 0.0f});                 // trim away the cap right of the chord
    ASSERT_EQ(sk.getCircles().size(), 0u) << "the circle became an arc";
    ASSERT_EQ(sk.getArcs().size(), 1u);
    const int arcId = sk.getArcs().front().id;

    Rig r(sk);
    EXPECT_FALSE(r.picksArc({10.0f, 0.0f}, arcId)) << "where the trimmed cap used to be";
    EXPECT_TRUE(r.picksArc({-10.0f, 0.0f}, arcId)) << "on the arc that is left";
}
