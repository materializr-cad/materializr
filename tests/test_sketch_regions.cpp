// Regression: interior selection of a multi-loop sketch. Two disjoint
// triangles produced BOP region faces whose wires contain TopAbs_REVERSED
// edges; densifyWire2D sampled those f->l regardless of orientation, so the
// densified polygon self-intersected and the even-odd point-in-polygon test
// miscounted - interior clicks missed and the loop was only selectable near
// its edges. densifyWire2D now honours the wire traversal orientation.
//
// (Mirrors the user's Sketch 3: triangle A (90,90)-(90,55)-(25,90) and
//  triangle B (90,35)-(90,0)-(25,0), both with an edge on x=90.)

#include "modeling/Sketch.h"

#include <gtest/gtest.h>
#include <gp_Pln.hxx>
#include <gp_Ax3.hxx>
#include <glm/glm.hpp>
#include <vector>

using materializr::Sketch;

namespace {

// A triangle from three (x,y) sketch-plane corners. Returns nothing; adds to sk.
void addTriangle(Sketch& sk, glm::vec2 a, glm::vec2 b, glm::vec2 c) {
    int pa = sk.addPoint(a), pb = sk.addPoint(b), pc = sk.addPoint(c);
    sk.addLine(pa, pb);
    sk.addLine(pb, pc);
    sk.addLine(pc, pa);
}

// Centroid of a triangle - guaranteed strictly interior.
glm::vec2 centroid(glm::vec2 a, glm::vec2 b, glm::vec2 c) {
    return (a + b + c) / 3.0f;
}

} // namespace

TEST(SketchRegions, TwoTrianglesInteriorSelectable) {
    Sketch sk;
    sk.setPlane(gp_Pln(gp_Ax3(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0))));

    const glm::vec2 a0(90, 90), a1(90, 55), a2(25, 90); // triangle A
    const glm::vec2 b0(90, 35), b1(90, 0),  b2(25, 0);  // triangle B
    addTriangle(sk, a0, a1, a2);
    addTriangle(sk, b0, b1, b2);

    auto regions = sk.buildRegions();
    ASSERT_EQ(regions.size(), 2u) << "two disjoint triangles = two fill regions";

    // Each triangle's centroid must land INSIDE exactly one region via the
    // strict interior test (not merely the edge-proximity fallback).
    const glm::vec2 cA = centroid(a0, a1, a2);
    const glm::vec2 cB = centroid(b0, b1, b2);

    auto interiorHits = [&](glm::vec2 p) {
        int n = 0;
        for (const auto& r : regions) if (sk.isPointInRegion(r, p)) ++n;
        return n;
    };
    EXPECT_EQ(interiorHits(cA), 1) << "triangle A centroid must be strictly inside its region";
    EXPECT_EQ(interiorHits(cB), 1) << "triangle B centroid must be strictly inside its region";

    // A point clearly outside both triangles is inside neither.
    EXPECT_EQ(interiorHits(glm::vec2(0, 45)), 0);
}

// Single triangle sanity - the simple case must keep working.
TEST(SketchRegions, SingleTriangleInterior) {
    Sketch sk;
    sk.setPlane(gp_Pln(gp_Ax3(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0))));
    const glm::vec2 a(0, 0), b(60, 0), c(0, 40);
    addTriangle(sk, a, b, c);

    auto regions = sk.buildRegions();
    ASSERT_EQ(regions.size(), 1u);
    EXPECT_TRUE(sk.isPointInRegion(regions[0], centroid(a, b, c)));
    EXPECT_FALSE(sk.isPointInRegion(regions[0], glm::vec2(50, 35))); // outside hypotenuse
}

// Framing bounds must NOT include an arc's far centre. A subtle (nearly flat)
// arc lies on a huge circle whose centre is far from the drawn geometry;
// including it made "frame sketch" / sketch-entry zoom out to nothing
// (Steve's report). The centre POINT still exists and stays interactive -
// this only governs the camera box.
TEST(SketchBounds, SubtleArcCentreExcludedFromFraming) {
    using materializr::Sketch;
    Sketch sk;
    sk.setPlane(gp_Pln(gp_Ax3(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0))));
    // A shallow arc from (-10,0) to (10,0) bulging up ~0.5mm: radius ~100mm,
    // so the circle centre sits ~99.5mm BELOW the chord.
    const double r = 100.25;
    const double cy = 0.5 - r;                 // centre far below
    int cId = sk.addPoint({0.0f, (float)cy});
    // Start on the RIGHT, end on the LEFT: the buildWires CCW start->end
    // convention then traces the SHORT way over the top (the shallow bulge),
    // which is what a drawn subtle arc actually is.
    int sId = sk.addPoint({10.0f, 0.0f});
    int eId = sk.addPoint({-10.0f, 0.0f});
    sk.addArc(cId, sId, eId, r);

    glm::vec3 lo, hi;
    ASSERT_TRUE(sk.getWorldBounds(lo, hi));
    // The drawn arc spans x in [-10,10], y in [0, ~0.5]. The bounds must hug
    // that, NOT stretch ~100mm down to the centre.
    EXPECT_NEAR(lo.x, -10.0f, 0.5f);
    EXPECT_NEAR(hi.x,  10.0f, 0.5f);
    EXPECT_GT(lo.y, -2.0f) << "bounds must not reach the far arc centre";
    EXPECT_NEAR(hi.y, 0.5f, 0.2f) << "arc bulge enclosed";
    // Sanity: overall box height is small, not ~100mm.
    EXPECT_LT(hi.y - lo.y, 3.0f);
}

// A point sitting exactly on a circle's perimeter (a line endpoint snapped to
// the rim, or the circle's own drag-release point) must NOT dissolve the
// circle's loop. Regression for Steve's report: a 12mm circle concentric in a
// 20mm circle, with a point on the 20mm rim, made the annulus between them
// unselectable (the outer loop never formed).
TEST(SketchRegions, RimPointKeepsConcentricAnnulus) {
    using materializr::Sketch;
    Sketch sk;
    sk.setPlane(gp_Pln(gp_Ax3(gp_Pnt(0,0,0), gp_Dir(0,0,1), gp_Dir(1,0,0))));
    int c = sk.addPoint({4.0f, -4.0f});
    sk.addCircle(c, 6.0);
    sk.addCircle(c, 10.0499);
    sk.addPoint({4.0f, -14.0499f});         // exactly on the outer rim
    auto regions = sk.buildRegions();
    ASSERT_EQ(regions.size(), 2u) << "inner disk + annulus must both survive";
    // A click in the annular band (radius 8 from centre) must hit a region.
    bool bandHit = false;
    for (const auto& r : regions)
        if (sk.isPointInRegion(r, glm::vec2(-4.0f, -4.0f))) bandHit = true;
    EXPECT_TRUE(bandHit) << "the ring between the circles must be selectable";
}
