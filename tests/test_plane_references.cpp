// Snap references for a sketch on a plane: geometry already lying ON that plane
// (body edges and vertices, the cross-section where the plane cuts a body, and
// points/lines of other sketches on the same plane) becomes snappable on sketch
// entry, without anything being drawn first.

#include "modeling/PlaneReferences.h"
#include "modeling/Sketch.h"

#include <gtest/gtest.h>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <gp_Ax2.hxx>
#include <gp_Ax3.hxx>
#include <gp_Pln.hxx>

using namespace materializr;

namespace {

Sketch sketchAtZ(double z) {
    Sketch sk;
    sk.setPlane(gp_Pln(gp_Ax3(gp_Pnt(0, 0, z), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0))));
    return sk;
}

bool hasPoint(const Sketch& sk, float x, float y) {
    for (const auto& p : sk.getFaceReferences().points)
        if (std::abs(p.x - x) < 1e-3f && std::abs(p.y - y) < 1e-3f) return true;
    return false;
}

} // namespace

TEST(PlaneReferences, TopFaceOfABoxGivesItsOutline) {
    TopoDS_Shape box = BRepPrimAPI_MakeBox(10, 10, 10).Shape();
    Sketch sk = sketchAtZ(10.0);
    auto st = addBodyPlaneReferences(sk, {box});
    EXPECT_FALSE(st.crossSection) << "the plane only touches the box";
    EXPECT_EQ(sk.getFaceReferences().lines.size(), 4u);
    EXPECT_TRUE(hasPoint(sk, 0, 0));
    EXPECT_TRUE(hasPoint(sk, 10, 10));
    EXPECT_TRUE(hasPoint(sk, 5, 0)) << "edge midpoints are snap points too";
}

TEST(PlaneReferences, PlaneThroughABodyGivesItsCrossSection) {
    TopoDS_Shape box = BRepPrimAPI_MakeBox(10, 20, 10).Shape();
    Sketch sk = sketchAtZ(5.0);
    auto st = addBodyPlaneReferences(sk, {box});
    EXPECT_TRUE(st.crossSection);
    EXPECT_EQ(sk.getFaceReferences().lines.size(), 4u);
    EXPECT_TRUE(hasPoint(sk, 10, 20));
}

TEST(PlaneReferences, CrossSectionOfACylinderIsATrueCircle) {
    TopoDS_Shape cyl = BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(3, 4, 0), gp_Dir(0, 0, 1)), 5.0, 10.0).Shape();
    Sketch sk = sketchAtZ(5.0);
    addBodyPlaneReferences(sk, {cyl});
    ASSERT_FALSE(sk.getFaceReferences().circles.empty());
    const auto& c = sk.getFaceReferences().circles.front();
    EXPECT_NEAR(c.radius, 5.0f, 1e-3f);
    EXPECT_NEAR(c.center.x, 3.0f, 1e-3f);
    EXPECT_NEAR(c.center.y, 4.0f, 1e-3f);
}

TEST(PlaneReferences, ABodyThePlaneDoesNotReachContributesNothing) {
    TopoDS_Shape box = BRepPrimAPI_MakeBox(10, 10, 10).Shape();
    Sketch sk = sketchAtZ(25.0);
    addBodyPlaneReferences(sk, {box});
    EXPECT_TRUE(sk.getFaceReferences().points.empty());
    EXPECT_TRUE(sk.getFaceReferences().lines.empty());
}

TEST(PlaneReferences, MergingTwiceDoesNotDuplicate) {
    TopoDS_Shape box = BRepPrimAPI_MakeBox(10, 10, 10).Shape();
    Sketch sk = sketchAtZ(10.0);
    addBodyPlaneReferences(sk, {box});
    const size_t pts = sk.getFaceReferences().points.size();
    const size_t lns = sk.getFaceReferences().lines.size();
    auto again = addBodyPlaneReferences(sk, {box});
    EXPECT_EQ(sk.getFaceReferences().points.size(), pts);
    EXPECT_EQ(sk.getFaceReferences().lines.size(), lns);
    EXPECT_EQ(again.points, 0);
}

TEST(PlaneReferences, OtherSketchOnTheSamePlaneIsSnappable) {
    Sketch other = sketchAtZ(0.0);
    const int a = other.addPoint({2, 3}), b = other.addPoint({12, 3});
    other.addLine(a, b);
    other.addCircle(other.addPoint({7, 8}), 2.5);

    Sketch sk = sketchAtZ(0.0);
    auto st = addSketchPlaneReferences(sk, {&other});
    EXPECT_GE(st.points, 3);
    EXPECT_TRUE(hasPoint(sk, 2, 3));
    EXPECT_TRUE(hasPoint(sk, 12, 3));
    EXPECT_TRUE(hasPoint(sk, 7, 3)) << "midpoint of the other sketch's line";
    ASSERT_EQ(sk.getFaceReferences().circles.size(), 1u);
    EXPECT_NEAR(sk.getFaceReferences().circles[0].radius, 2.5f, 1e-4f);
}

TEST(PlaneReferences, SketchOnAnotherPlaneIsIgnored) {
    Sketch other = sketchAtZ(5.0);
    other.addLine(other.addPoint({0, 0}), other.addPoint({10, 0}));
    Sketch sk = sketchAtZ(0.0);
    addSketchPlaneReferences(sk, {&other});
    EXPECT_TRUE(sk.getFaceReferences().points.empty());
}

TEST(PlaneReferences, OtherSketchInDifferentOriginIsMappedIntoThisFrame) {
    // The other sketch's origin is offset, so its (0,0) is (10,10) here.
    Sketch other;
    other.setPlane(gp_Pln(gp_Ax3(gp_Pnt(10, 10, 0), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0))));
    other.addPoint({1, 2});
    Sketch sk = sketchAtZ(0.0);
    addSketchPlaneReferences(sk, {&other});
    EXPECT_TRUE(hasPoint(sk, 11, 12));
}
