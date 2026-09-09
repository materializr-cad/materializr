// Regression: a push/pull cut deeper than the body consumed the ENTIRE solid -
// BRepAlgoAPI_Cut returns an empty compound, execute() stored it via
// updateBody, tessellation then failed every preview frame (stale mesh) and
// the body vanished on commit. A cut that would leave no solid must be
// REFUSED (document untouched); a partial cut must keep working.

#include "core/Document.h"
#include "modeling/PushPullOp.h"

#include <gtest/gtest.h>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepGProp.hxx>
#include <BRepGProp_Face.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <Geom_Plane.hxx>
#include <Geom_Surface.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <cmath>

namespace {

// The planar face whose outward normal is ~+Z at the top of the shape.
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

double solidVolume(const TopoDS_Shape& s) {
    GProp_GProps gp;
    BRepGProp::VolumeProperties(s, gp);
    return gp.Mass();
}

bool hasSolid(const TopoDS_Shape& s) {
    return !s.IsNull() && TopExp_Explorer(s, TopAbs_SOLID).More();
}

} // namespace

TEST(PushPullThrough, CutConsumingWholeBodyIsRefused) {
    Document doc;
    TopoDS_Shape box = BRepPrimAPI_MakeBox(gp_Pnt(0,0,0), 20, 20, 10).Shape();
    int id = doc.addBody(box, "Box");
    const double vol0 = solidVolume(doc.getBody(id));
    ASSERT_GT(vol0, 1.0);

    TopoDS_Face top = topFace(doc.getBody(id), 10.0);
    ASSERT_FALSE(top.IsNull());

    PushPullOp op;
    std::vector<PushPullOp::Target> targets(1);
    targets[0].profile = top;
    targets[0].sourceBodyId = id;
    op.setTargets(std::move(targets));
    op.setDistance(-50.0); // cut 5x deeper than the body - would consume it all
    op.setCutIntersecting(true); // app sets this for every cut direction

    const bool applied = op.execute(doc);

    // The document must still hold the intact solid - never an empty shape.
    TopoDS_Shape after = doc.getBody(id);
    EXPECT_TRUE(hasSolid(after))
        << "cut-through stored a solid-less shape (the vanishing-body bug)";
    EXPECT_NEAR(solidVolume(after), vol0, 1e-6)
        << "refused cut must leave the body untouched";
    EXPECT_FALSE(applied) << "an all-consuming cut must report failure so "
                             "History::pushOperation rejects it";
}

TEST(PushPullThrough, PartialCutStillWorks) {
    Document doc;
    TopoDS_Shape box = BRepPrimAPI_MakeBox(gp_Pnt(0,0,0), 20, 20, 10).Shape();
    int id = doc.addBody(box, "Box");
    const double vol0 = solidVolume(doc.getBody(id));

    TopoDS_Face top = topFace(doc.getBody(id), 10.0);
    ASSERT_FALSE(top.IsNull());

    PushPullOp op;
    std::vector<PushPullOp::Target> targets(1);
    targets[0].profile = top;
    targets[0].sourceBodyId = id;
    op.setTargets(std::move(targets));
    op.setDistance(-5.0); // pocket half way - legitimate
    op.setCutIntersecting(true);

    ASSERT_TRUE(op.execute(doc));
    TopoDS_Shape after = doc.getBody(id);
    ASSERT_TRUE(hasSolid(after));
    EXPECT_LT(solidVolume(after), vol0 - 1.0) << "partial cut must remove material";
    EXPECT_GT(solidVolume(after), 1.0);
}
