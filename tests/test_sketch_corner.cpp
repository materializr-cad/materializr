// Sketch fillet / chamfer: round or bevel a corner where two lines meet
// (feature request 11-6-26 - until now a sketch fillet meant drawing circles
// and trimming away the excess by hand).
#include <gtest/gtest.h>

#include "modeling/Sketch.h"

#include <cmath>
#include <glm/glm.hpp>

using materializr::Sketch;

namespace {
// A 10 x 10 square on the origin; returns the id of the point at (0, 0).
int squareWithOriginCorner(Sketch& sk) {
    sk.addRectangle({0, 0}, {10, 10});
    for (const auto& p : sk.getPoints())
        if (glm::length(p.pos) < 1e-6f) return p.id;
    return -1;
}
int pointNear(const Sketch& sk, glm::vec2 q) {
    for (const auto& p : sk.getPoints())
        if (glm::length(p.pos - q) < 1e-4f) return p.id;
    return -1;
}
}

TEST(SketchCorner, FilletReplacesTheCornerWithATangentArc) {
    Sketch sk;
    const int corner = squareWithOriginCorner(sk);
    ASSERT_GE(corner, 0);
    ASSERT_TRUE(sk.isRoundableCorner(corner));

    std::vector<int> made;
    ASSERT_TRUE(sk.roundCorner(corner, 2.0, /*chamfer=*/false, &made));
    EXPECT_EQ(sk.getPoint(corner), nullptr) << "the corner point is gone";
    ASSERT_EQ(sk.getArcs().size(), 1u);
    EXPECT_EQ(sk.getLines().size(), 4u);

    // Square corner, radius 2: tangent points 2 along each side, centre (2, 2).
    EXPECT_GE(pointNear(sk, {2, 0}), 0);
    EXPECT_GE(pointNear(sk, {0, 2}), 0);
    const auto& arc = sk.getArcs().front();
    const auto* c = sk.getPoint(arc.centerPointId);
    ASSERT_NE(c, nullptr);
    EXPECT_NEAR(c->pos.x, 2.0f, 1e-4f);
    EXPECT_NEAR(c->pos.y, 2.0f, 1e-4f);
    EXPECT_NEAR(arc.radius, 2.0, 1e-9);

    // CCW from start to end must be the SHORT way (a quarter turn), not the
    // three-quarter complement.
    const auto* s = sk.getPoint(arc.startPointId);
    const auto* e = sk.getPoint(arc.endPointId);
    double sweep = std::atan2(e->pos.y - c->pos.y, e->pos.x - c->pos.x) -
                   std::atan2(s->pos.y - c->pos.y, s->pos.x - c->pos.x);
    while (sweep <= 0.0) sweep += 2.0 * M_PI;
    EXPECT_NEAR(sweep, M_PI * 0.5, 1e-3);

    // Still one closed contour.
    EXPECT_EQ(sk.buildRegions().size(), 1u);
}

TEST(SketchCorner, ChamferJoinsTheTwoSetbackPointsWithALine) {
    Sketch sk;
    const int corner = squareWithOriginCorner(sk);
    ASSERT_TRUE(sk.roundCorner(corner, 3.0, /*chamfer=*/true));
    EXPECT_TRUE(sk.getArcs().empty());
    EXPECT_EQ(sk.getLines().size(), 5u);
    const int a = pointNear(sk, {3, 0}), b = pointNear(sk, {0, 3});
    ASSERT_GE(a, 0);
    ASSERT_GE(b, 0);
    bool bevel = false;
    for (const auto& l : sk.getLines())
        if ((l.startPointId == a && l.endPointId == b) || (l.startPointId == b && l.endPointId == a))
            bevel = true;
    EXPECT_TRUE(bevel);
    EXPECT_EQ(sk.buildRegions().size(), 1u);
}

TEST(SketchCorner, ASizeThatDoesNotFitChangesNothing) {
    Sketch sk;
    const int corner = squareWithOriginCorner(sk);
    const size_t pts = sk.getPoints().size(), lines = sk.getLines().size();
    EXPECT_FALSE(sk.roundCorner(corner, 10.5, false));
    EXPECT_FALSE(sk.roundCorner(corner, 10.5, true));
    EXPECT_FALSE(sk.roundCorner(corner, 0.0, false));
    EXPECT_EQ(sk.getPoints().size(), pts);
    EXPECT_EQ(sk.getLines().size(), lines);
    EXPECT_NE(sk.getPoint(corner), nullptr);
}

TEST(SketchCorner, OnlyATwoLineCornerQualifies) {
    Sketch sk;
    const int a = sk.addPoint({0, 0}), b = sk.addPoint({10, 0}), c = sk.addPoint({20, 0});
    sk.addLine(a, b);
    EXPECT_FALSE(sk.isRoundableCorner(a)) << "an end of a single line";
    sk.addLine(b, c);
    EXPECT_FALSE(sk.roundCorner(b, 1.0, false)) << "two collinear lines make no corner";
    const int d = sk.addPoint({10, 10});
    sk.addLine(b, d);
    EXPECT_FALSE(sk.isRoundableCorner(b)) << "three lines meet here";
}

TEST(SketchCorner, AllFourCornersCanBeRounded) {
    Sketch sk;
    squareWithOriginCorner(sk);
    std::vector<int> corners;
    for (const auto& p : sk.getPoints()) corners.push_back(p.id);
    for (int id : corners) ASSERT_TRUE(sk.roundCorner(id, 1.5, false));
    EXPECT_EQ(sk.getArcs().size(), 4u);
    EXPECT_EQ(sk.getLines().size(), 4u);
    EXPECT_EQ(sk.buildRegions().size(), 1u);
}
