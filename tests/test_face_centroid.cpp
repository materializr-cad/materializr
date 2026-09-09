// Regression: the face-centroid snap (the green centre marker, and the snap
// that lands on it) must follow the sketch when the sketch is moved.
//
// Steve, 2026-07-31, looking at an irregular face: "the center marker might be
// off." Two of the three things found were by design - the marker is the face's
// AREA centroid, so on an irregular face it isn't the middle of the bounding
// box, and on a concave face it can sit outside the material; and holes count,
// so an off-centre pocket pulls it away from itself.
//
// The third was a real bug. The centroid was cached in PLANE-RELATIVE 2D and
// invalidated only by setSourceFace - but moving a sketch calls setPlane, which
// had no way to know the cache existed. The marker stayed behind by exactly the
// distance moved. It is cached in 3D now (the centroid belongs to the face, not
// to the plane) and projected on read.

#include "modeling/Sketch.h"

#include <gtest/gtest.h>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <gp_Ax1.hxx>
#include <gp_Ax3.hxx>
#include <gp_Dir.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>
#include <cmath>

using materializr::Sketch;

namespace {

gp_Pln xyPlane() {
    return gp_Pln(gp_Ax3(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0)));
}

// An L-shaped face at z=0: a 20x20 square with the top-right 10x10 removed.
// Three 10x10 quadrants, so the area centroid is (25/3, 25/3) - NOT the
// bounding-box centre (10, 10), which is the notch corner.
TopoDS_Face lFace() {
    BRepBuilderAPI_MakePolygon poly;
    poly.Add(gp_Pnt(0, 0, 0));
    poly.Add(gp_Pnt(20, 0, 0));
    poly.Add(gp_Pnt(20, 10, 0));
    poly.Add(gp_Pnt(10, 10, 0));
    poly.Add(gp_Pnt(10, 20, 0));
    poly.Add(gp_Pnt(0, 20, 0));
    poly.Close();
    return BRepBuilderAPI_MakeFace(poly.Wire()).Face();
}

} // namespace

TEST(FaceCentroid, IsTheAreaCentroid) {
    Sketch sk;
    sk.setPlane(xyPlane());
    sk.setSourceFace(lFace());

    glm::vec2 c;
    ASSERT_TRUE(sk.getSourceFaceCentroid(c));
    EXPECT_NEAR(c.x, 25.0 / 3.0, 1e-3);
    EXPECT_NEAR(c.y, 25.0 / 3.0, 1e-3);
}

TEST(FaceCentroid, FollowsAMovedSketchPlane) {
    Sketch sk;
    sk.setPlane(xyPlane());
    sk.setSourceFace(lFace());

    glm::vec2 before;
    ASSERT_TRUE(sk.getSourceFaceCentroid(before)); // populates the cache

    // Move the sketch 5mm in +X the way SketchTransformOp / the gizmo drag do:
    // setPlane alone. The source face stays where it is.
    gp_Trsf t;
    t.SetTranslation(gp_Vec(5, 0, 0));
    sk.setPlane(sk.getPlane().Transformed(t));

    glm::vec2 after;
    ASSERT_TRUE(sk.getSourceFaceCentroid(after));

    // The face didn't move, so in the plane's new frame the centroid is 5mm
    // further back along +X. A cached 2D value would have reported `before`.
    EXPECT_NEAR(after.x, before.x - 5.0f, 1e-3)
        << "the centroid did not follow the moved sketch plane";
    EXPECT_NEAR(after.y, before.y, 1e-3);

    // And it agrees with a from-scratch computation in the same frame.
    Sketch fresh;
    fresh.setPlane(sk.getPlane());
    fresh.setSourceFace(lFace());
    glm::vec2 truth;
    ASSERT_TRUE(fresh.getSourceFaceCentroid(truth));
    EXPECT_NEAR(after.x, truth.x, 1e-4);
    EXPECT_NEAR(after.y, truth.y, 1e-4);
}

// Rotating the sketch in its own plane moves the 2D frame too.
TEST(FaceCentroid, FollowsARotatedSketchPlane) {
    Sketch sk;
    sk.setPlane(xyPlane());
    sk.setSourceFace(lFace());
    glm::vec2 before;
    ASSERT_TRUE(sk.getSourceFaceCentroid(before));

    gp_Trsf r;
    r.SetRotation(gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), M_PI / 2.0);
    sk.setPlane(sk.getPlane().Transformed(r));

    glm::vec2 after;
    ASSERT_TRUE(sk.getSourceFaceCentroid(after));

    Sketch fresh;
    fresh.setPlane(sk.getPlane());
    fresh.setSourceFace(lFace());
    glm::vec2 truth;
    ASSERT_TRUE(fresh.getSourceFaceCentroid(truth));
    EXPECT_NEAR(after.x, truth.x, 1e-4);
    EXPECT_NEAR(after.y, truth.y, 1e-4);
}

// Replacing the face still recomputes (the 3D cache must not outlive it).
TEST(FaceCentroid, RecomputesWhenTheFaceIsReplaced) {
    Sketch sk;
    sk.setPlane(xyPlane());
    sk.setSourceFace(lFace());
    glm::vec2 lShape;
    ASSERT_TRUE(sk.getSourceFaceCentroid(lShape));

    BRepBuilderAPI_MakePolygon square;
    square.Add(gp_Pnt(0, 0, 0));
    square.Add(gp_Pnt(20, 0, 0));
    square.Add(gp_Pnt(20, 20, 0));
    square.Add(gp_Pnt(0, 20, 0));
    square.Close();
    sk.setSourceFace(BRepBuilderAPI_MakeFace(square.Wire()).Face());

    glm::vec2 sq;
    ASSERT_TRUE(sk.getSourceFaceCentroid(sq));
    EXPECT_NEAR(sq.x, 10.0, 1e-3) << "stale centroid from the previous face";
    EXPECT_NEAR(sq.y, 10.0, 1e-3);
}
