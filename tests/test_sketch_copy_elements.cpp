// #182: the sketch Copy tool duplicated only points and straight lines, so a
// circle, arc, polygon or spline vanished from the copy. Sketch::copyElements
// duplicates every element type, keeps connected geometry connected, and keeps
// the construction flag.

#include <gtest/gtest.h>

#include "modeling/Sketch.h"

#include <glm/glm.hpp>
#include <cmath>

using materializr::Sketch;

namespace {
const float kPi = 3.14159265f;

glm::vec2 posOf(const Sketch& sk, int pid) { return sk.getPoint(pid)->pos; }

// One of everything: a line, a circle, an arc, a spline, a 4-sided "polygon"
// (4 lines on shared points) and a loose point.
struct Scene {
    Sketch sk;
    int line = -1, circle = -1, arc = -1, spline = -1, loose = -1;
    std::vector<int> polyLines;
    Scene() {
        int a = sk.addPoint({0, 0}), b = sk.addPoint({10, 0});
        line = sk.addLine(a, b);
        int cc = sk.addPoint({30, 0});
        circle = sk.addCircle(cc, 5.0);
        int ac = sk.addPoint({0, 30}), as = sk.addPoint({10, 30}), ae = sk.addPoint({0, 40});
        arc = sk.addArc(ac, as, ae, 10.0);
        int s0 = sk.addPoint({40, 30}), s1 = sk.addPoint({45, 38}), s2 = sk.addPoint({52, 30}),
            s3 = sk.addPoint({58, 36});
        spline = sk.addSpline({s0, s1, s2, s3});
        int q0 = sk.addPoint({60, 0}), q1 = sk.addPoint({70, 0}), q2 = sk.addPoint({70, 10}),
            q3 = sk.addPoint({60, 10});
        polyLines = {sk.addLine(q0, q1), sk.addLine(q1, q2), sk.addLine(q2, q3), sk.addLine(q3, q0)};
        loose = sk.addPoint({90, 90});
    }
};
}

TEST(SketchCopyElements, EveryElementTypeIsDuplicated) {
    Scene s;
    const auto before = s.sk.allElementIds();
    const size_t nLines = s.sk.getLines().size(), nCircles = s.sk.getCircles().size(),
                 nArcs = s.sk.getArcs().size(), nSplines = s.sk.getSplines().size();

    const auto made = s.sk.copyElements(before, {3.0f, 4.0f});

    EXPECT_EQ(s.sk.getLines().size(),   nLines * 2)   << "the line and the polygon's sides";
    EXPECT_EQ(s.sk.getCircles().size(), nCircles * 2);
    EXPECT_EQ(s.sk.getArcs().size(),    nArcs * 2);
    EXPECT_EQ(s.sk.getSplines().size(), nSplines * 2);
    EXPECT_EQ(made.lines.size(), nLines);
    EXPECT_EQ(made.circles.size(), 1u);
    EXPECT_EQ(made.arcs.size(), 1u);
    EXPECT_EQ(made.splines.size(), 1u);
}

TEST(SketchCopyElements, CopiesAreShiftedByTheOffset) {
    Scene s;
    Sketch::ElementIds one; one.circles.insert(s.circle);
    const auto made = s.sk.copyElements(one, {3.0f, 4.0f});
    ASSERT_EQ(made.circles.size(), 1u);
    const auto& copy = s.sk.getCircles().back();
    EXPECT_EQ(copy.id, *made.circles.begin());
    EXPECT_NEAR(copy.radius, 5.0, 1e-9);
    EXPECT_NEAR(posOf(s.sk, copy.centerPointId).x, 33.0f, 1e-5f);
    EXPECT_NEAR(posOf(s.sk, copy.centerPointId).y, 4.0f, 1e-5f);
}

TEST(SketchCopyElements, ArcKeepsItsSweepAndPicksUpItsEndPoints) {
    Scene s;
    Sketch::ElementIds one; one.arcs.insert(s.arc);      // only the arc id: points come with it
    const auto made = s.sk.copyElements(one, {5.0f, 0.0f});
    ASSERT_EQ(made.arcs.size(), 1u);
    const materializr::SketchArc* copy = nullptr;
    for (const auto& a : s.sk.getArcs()) if (a.id == *made.arcs.begin()) copy = &a;
    ASSERT_NE(copy, nullptr);
    EXPECT_NEAR(posOf(s.sk, copy->startPointId).x, 15.0f, 1e-5f);
    EXPECT_NEAR(posOf(s.sk, copy->endPointId).y, 40.0f, 1e-5f);
    EXPECT_EQ(made.points.size(), 3u) << "centre, start and end";
}

TEST(SketchCopyElements, SplineKeepsAllItsControlPoints) {
    Scene s;
    Sketch::ElementIds one; one.splines.insert(s.spline);
    const auto made = s.sk.copyElements(one, {0.0f, -20.0f});
    ASSERT_EQ(made.splines.size(), 1u);
    const materializr::SketchSpline* copy = nullptr;
    for (const auto& sp : s.sk.getSplines()) if (sp.id == *made.splines.begin()) copy = &sp;
    ASSERT_NE(copy, nullptr);
    ASSERT_EQ(copy->controlPointIds.size(), 4u);
    EXPECT_NEAR(posOf(s.sk, copy->controlPointIds[1]).x, 45.0f, 1e-5f);
    EXPECT_NEAR(posOf(s.sk, copy->controlPointIds[1]).y, 18.0f, 1e-5f);
}

TEST(SketchCopyElements, ConnectedSidesStayConnected) {
    Scene s;
    Sketch::ElementIds poly;
    for (int l : s.polyLines) poly.lines.insert(l);
    const size_t pointsBefore = s.sk.getPoints().size();
    const auto made = s.sk.copyElements(poly, {0.0f, 20.0f});
    ASSERT_EQ(made.lines.size(), 4u);
    EXPECT_EQ(s.sk.getPoints().size(), pointsBefore + 4) << "4 shared corners, not 8 endpoints";
}

TEST(SketchCopyElements, ConstructionFlagIsKept) {
    Scene s;
    s.sk.setElementConstruction(s.circle, true);
    s.sk.setElementConstruction(s.spline, true);
    Sketch::ElementIds some; some.circles.insert(s.circle); some.splines.insert(s.spline);
    const auto made = s.sk.copyElements(some, {1.0f, 1.0f});
    for (const auto& c : s.sk.getCircles())
        if (made.circles.count(c.id)) EXPECT_TRUE(c.isConstruction);
    for (const auto& sp : s.sk.getSplines())
        if (made.splines.count(sp.id)) EXPECT_TRUE(sp.isConstruction);
}

TEST(SketchCopyElements, LoosePointIsCopiedAndNothingElseChanges) {
    Scene s;
    Sketch::ElementIds one; one.points.insert(s.loose);
    const size_t lines = s.sk.getLines().size();
    const auto made = s.sk.copyElements(one, {2.0f, 2.0f});
    EXPECT_EQ(made.points.size(), 1u);
    EXPECT_EQ(s.sk.getLines().size(), lines);
}

TEST(SketchCopyElements, CopyDoesNotStayStuckToTheOriginalsRim) {
    Sketch sk;
    int c = sk.addPoint({0, 0});
    int circ = sk.addCircle(c, 10.0);
    int onRim = sk.addPoint({10, 0});
    sk.setPointOnCurve(onRim, circ);
    Sketch::ElementIds some; some.points.insert(onRim);
    const auto made = sk.copyElements(some, {5.0f, 5.0f});
    ASSERT_EQ(made.points.size(), 1u);
    EXPECT_LT(sk.getPoint(*made.points.begin())->onCurveId, 0);
}
