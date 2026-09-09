// Regression: push/pull ran BACKWARDS on a holed (annular) top face that was
// orientation-reversed in a saved project. correctedOutwardNormal probed the
// face's parametric centre - which on an annular face sits inside the hole - so
// both ±ε classifier probes read OUTSIDE, the verdict was "ambiguous", and the
// reversed (inward) normal was returned unchanged → the push/pull arrow + sign
// inverted. The fix probes a point sampled ON the face material instead.

#include "core/Document.h"
#include "modeling/PushPullOp.h" // declares correctedOutwardNormal

#include <gtest/gtest.h>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepGProp_Face.hxx>
#include <BRep_Tool.hxx>
#include <Geom_Surface.hxx>
#include <Geom_Plane.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <gp_Ax2.hxx>
#include <cmath>

namespace {
// The planar top face (z≈thickness, normal ~±Z).
TopoDS_Face topFace(const TopoDS_Shape& s, double zTop) {
    for (TopExp_Explorer ex(s, TopAbs_FACE); ex.More(); ex.Next()) {
        TopoDS_Face f = TopoDS::Face(ex.Current());
        Handle(Geom_Surface) surf = BRep_Tool::Surface(f);
        if (surf.IsNull() || !surf->IsKind(STANDARD_TYPE(Geom_Plane))) continue;
        BRepGProp_Face g(f); double u1,u2,v1,v2; g.Bounds(u1,u2,v1,v2);
        gp_Pnt c; gp_Vec n; g.Normal(0.5*(u1+u2),0.5*(v1+v2),c,n);
        if (std::abs(n.Z()) > 0.9 && c.Z() > zTop - 0.1) return f;
    }
    return TopoDS_Face();
}
} // namespace

TEST(PushPullNormal, AnnularReversedTopFaceCorrectsOutward) {
    // 20×20×2 plate with a big Ø16 hole → the top face is an annulus whose
    // parametric centre (10,10) falls INSIDE the hole.
    TopoDS_Shape plate = BRepPrimAPI_MakeBox(gp_Pnt(0,0,0), 20, 20, 2).Shape();
    TopoDS_Shape hole = BRepPrimAPI_MakeCylinder(
        gp_Ax2(gp_Pnt(10,10,-1), gp_Dir(0,0,1)), 8.0, 4.0).Shape();
    TopoDS_Shape part = BRepAlgoAPI_Cut(plate, hole).Shape();

    TopoDS_Face top = topFace(part, 2.0);
    ASSERT_FALSE(top.IsNull());

    // Force the bug's precondition: a REVERSED face → BRepGProp normal points
    // inward (−Z), exactly like the saved project's mis-oriented top face.
    TopoDS_Face rev = TopoDS::Face(top.Reversed());
    BRepGProp_Face g(rev); double u1,u2,v1,v2; g.Bounds(u1,u2,v1,v2);
    gp_Pnt c; gp_Vec n; g.Normal(0.5*(u1+u2),0.5*(v1+v2),c,n);
    ASSERT_LT(n.Z(), 0.0) << "reversed annular face normal points inward";

    gp_Vec outward = correctedOutwardNormal(part, rev, c, n);
    EXPECT_GT(outward.Z(), 0.0)
        << "must correct to true outward (+Z) even though the face centre is in "
           "the hole - else push/pull inverts on it";
}

TEST(PushPullNormal, SolidTopFaceUnchanged) {
    // A plain solid plate: the (correctly oriented) top normal must be left
    // outward, and a genuinely reversed one corrected - the common path still
    // works after the holed-face fix.
    TopoDS_Shape part = BRepPrimAPI_MakeBox(gp_Pnt(0,0,0), 20, 20, 2).Shape();
    TopoDS_Face top = topFace(part, 2.0);
    ASSERT_FALSE(top.IsNull());

    BRepGProp_Face g(top); double u1,u2,v1,v2; g.Bounds(u1,u2,v1,v2);
    gp_Pnt c; gp_Vec n; g.Normal(0.5*(u1+u2),0.5*(v1+v2),c,n);
    gp_Vec out = correctedOutwardNormal(part, top, c, n);
    EXPECT_GT(out.Z(), 0.0) << "correct outward normal stays outward";

    TopoDS_Face rev = TopoDS::Face(top.Reversed());
    BRepGProp_Face g2(rev); g2.Bounds(u1,u2,v1,v2);
    gp_Pnt c2; gp_Vec n2; g2.Normal(0.5*(u1+u2),0.5*(v1+v2),c2,n2);
    gp_Vec out2 = correctedOutwardNormal(part, rev, c2, n2);
    EXPECT_GT(out2.Z(), 0.0) << "reversed solid-face normal corrected outward";
}
