// Rung 4: generation-map ("gen") naming - the seed of the general kernel.
// A prism's side face is named by its DERIVATION (the profile edge that
// generated it, via OCCT's BRepBuilderAPI Generated() map), not by geometry.
// The decisive property: that name, minted on the ORIGINAL prism, resolves to
// the STRUCTURALLY CORRESPONDING face after the profile is edited - because the
// derivation structure is invariant even though the geometry changed. This is
// what will eventually cover blend/boolean/loft faces no sketch scheme can name.

#include "modeling/TopoName.h"
#include "modeling/GenerationLedger.h"

#include <gtest/gtest.h>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>
#include <cmath>

using namespace materializr;

namespace {

// Rectangle face on the XY plane: (0,0)-(w,0)-(w,h)-(0,h), closed.
TopoDS_Face rectFace(double w, double h) {
    BRepBuilderAPI_MakePolygon poly;
    poly.Add(gp_Pnt(0, 0, 0));
    poly.Add(gp_Pnt(w, 0, 0));
    poly.Add(gp_Pnt(w, h, 0));
    poly.Add(gp_Pnt(0, h, 0));
    poly.Close();
    return BRepBuilderAPI_MakeFace(poly.Wire()).Face();
}

struct FaceInfo { double area; gp_Pnt c; };
FaceInfo infoOf(const TopoDS_Face& f) {
    GProp_GProps g; BRepGProp::SurfaceProperties(f, g);
    return { g.Mass(), g.CentreOfMass() };
}

// The side wall of `prism` whose centroid is nearest (x,y) at mid-height.
TopoDS_Face sideWallNear(const TopoDS_Shape& prism, double x, double y) {
    TopoDS_Face best; double bestD = 1e18;
    for (TopExp_Explorer ex(prism, TopAbs_FACE); ex.More(); ex.Next()) {
        TopoDS_Face f = TopoDS::Face(ex.Current());
        FaceInfo fi = infoOf(f);
        if (std::abs(fi.c.Z() - 5.0) > 1e-6) continue;   // mid-height => vertical wall
        double d = std::hypot(fi.c.X() - x, fi.c.Y() - y);
        if (d < bestD) { bestD = d; best = f; }
    }
    return best;
}

} // namespace

TEST(TopoGen, PrismSideFaceNameSurvivesProfileEdit) {
    // Original prism from a 20x10 rectangle, extruded 10 in +Z.
    TopoDS_Face prof1 = rectFace(20.0, 10.0);
    BRepPrimAPI_MakePrism mk1(prof1, gp_Vec(0, 0, 10));
    TopoDS_Shape prism1 = mk1.Shape();
    topo::GenerationLedger led1;
    led1.capture(mk1, prof1, TopAbs_EDGE);   // profile EDGE -> side FACE
    ASSERT_GT(led1.generated.Extent(), 0);

    // Name the FRONT wall (y=0, centroid ~ (10,0,5)) by its lineage.
    TopoDS_Face front1 = sideWallNear(prism1, 10.0, 0.0);
    ASSERT_FALSE(front1.IsNull());
    topo::Context ctx1;
    ctx1.shape = prism1; ctx1.type = TopAbs_FACE; ctx1.gen = &led1;
    topo::Ref ref = topo::mint(front1, ctx1);
    ASSERT_FALSE(ref.empty());
    EXPECT_EQ(ref.names.front().scheme, "gen") << "gen is the most-robust scheme";

    // EDIT the profile: widen 20 -> 40. Rebuild the prism, fresh ledger.
    TopoDS_Face prof2 = rectFace(40.0, 10.0);
    BRepPrimAPI_MakePrism mk2(prof2, gp_Vec(0, 0, 10));
    TopoDS_Shape prism2 = mk2.Shape();
    topo::GenerationLedger led2;
    led2.capture(mk2, prof2, TopAbs_EDGE);

    // Resolve the ORIGINAL name against the NEW prism: it must land the front
    // wall of the widened prism (y=0), now spanning x 0..40 (area 40x10=400),
    // centroid ~ (20,0,5) - the structurally corresponding face.
    topo::Context ctx2;
    ctx2.shape = prism2; ctx2.type = TopAbs_FACE; ctx2.gen = &led2;
    TopoDS_Shape out;
    ASSERT_TRUE(topo::resolve(ref, ctx2, out))
        << "the lineage name must resolve on the edited prism";
    ASSERT_EQ(out.ShapeType(), TopAbs_FACE);

    FaceInfo fi = infoOf(TopoDS::Face(out));
    EXPECT_NEAR(fi.c.Y(), 0.0, 1e-6)  << "must be the FRONT wall (y=0)";
    EXPECT_NEAR(fi.c.X(), 20.0, 1e-6) << "centre of the now-40-wide wall";
    EXPECT_NEAR(fi.area, 400.0, 1e-6) << "40 x 10 wall";

    // And it is exactly the face the new ledger generates from the corresponding
    // profile edge (sanity: same face the geometry-independent path would pick).
    TopoDS_Face front2 = sideWallNear(prism2, 20.0, 0.0);
    EXPECT_TRUE(out.IsSame(front2));
}

TEST(TopoGen, NoLedgerMeansNoGenName) {
    // Without ctx.gen the gen scheme can't mint; the ref falls to the geometric
    // / ordinal schemes (here: ordinal, since there's no doc).
    TopoDS_Face prof = rectFace(20.0, 10.0);
    BRepPrimAPI_MakePrism mk(prof, gp_Vec(0, 0, 10));
    TopoDS_Shape prism = mk.Shape();
    TopoDS_Face front = sideWallNear(prism, 10.0, 0.0);
    ASSERT_FALSE(front.IsNull());

    topo::Context ctx;
    ctx.shape = prism; ctx.type = TopAbs_FACE;   // gen = nullptr
    topo::Ref ref = topo::mint(front, ctx);
    ASSERT_FALSE(ref.empty());
    for (const auto& n : ref.names) EXPECT_NE(n.scheme, "gen");
    EXPECT_EQ(ref.names.back().scheme, "ordinal");
}
