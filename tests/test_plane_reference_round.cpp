// The Reference tool on round bodies (1.8.0-beta.5 tester list).
//   * A sphere's section (and a pipe's wall circles) arrive split at the seam
//     into arcs; picking one copied half the outline. Arcs of one circle that
//     close it are one circle now.
//   * A symmetric body lost one of its two walls from the references: the
//     de-duplication hashed (-5,0)-(-5,10) and (5,0)-(5,10) to the same key.
#include "modeling/PlaneReferences.h"
#include "modeling/Sketch.h"

#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include <gp_Ax3.hxx>
#include <gp_Pln.hxx>
#include <gtest/gtest.h>
#include <cmath>

using namespace materializr;

namespace {
Sketch sketchOn(const gp_Pnt& o, const gp_Dir& n, const gp_Dir& x) {
    Sketch sk;
    sk.setPlane(gp_Pln(gp_Ax3(o, n, x)));
    return sk;
}
}

TEST(PlaneReferenceRound, SymmetricWallsBothSurvive) {
    TopoDS_Shape cyl = BRepPrimAPI_MakeCylinder(5.0, 10.0).Shape();   // axis +Z
    Sketch sk = sketchOn(gp_Pnt(0, 0, 0), gp_Dir(0, -1, 0), gp_Dir(1, 0, 0));   // XZ, through the axis
    addBodyPlaneReferences(sk, {cyl}, /*crossSections=*/true);
    int leftWall = 0, rightWall = 0;
    for (const auto& l : sk.getFaceReferences().lines) {
        const bool vertical = std::abs(l.first.x - l.second.x) < 1e-3f;
        if (!vertical) continue;
        if (std::abs(l.first.x + 5.0f) < 1e-3f) ++leftWall;
        if (std::abs(l.first.x - 5.0f) < 1e-3f) ++rightWall;
    }
    EXPECT_EQ(leftWall, 1);
    EXPECT_EQ(rightWall, 1) << "the mirror-image wall was dropped by a hash collision";
}

TEST(PlaneReferenceRound, SphereSectionIsOneFullCircle) {
    TopoDS_Shape sph = BRepPrimAPI_MakeSphere(30.0).Shape();
    // XZ and YZ planes both cut the sphere's seam; XY does not.
    const struct { gp_Dir n, x; } planes[] = {
        { gp_Dir(0, -1, 0), gp_Dir(1, 0, 0) },
        { gp_Dir(1, 0, 0),  gp_Dir(0, 1, 0) },
        { gp_Dir(0, 0, 1),  gp_Dir(1, 0, 0) },
    };
    for (const auto& p : planes) {
        Sketch sk = sketchOn(gp_Pnt(0, 0, 0), p.n, p.x);
        addBodyPlaneReferences(sk, {sph}, true);
        const auto& circles = sk.getFaceReferences().circles;
        ASSERT_EQ(circles.size(), 1u) << "normal (" << p.n.X() << "," << p.n.Y() << "," << p.n.Z() << ")";
        EXPECT_GE(circles.front().sweep, 2.0f * 3.14159265f - 1e-2f);
        EXPECT_NEAR(circles.front().radius, 30.0f, 1e-3f);
    }
}
