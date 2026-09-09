// MoveFaceOp Kind::Twist regression: twisting the top face of a prism about its
// normal produces a valid, correctly-twisted solid, preserves volume (a rigid
// per-section rotation adds/removes nothing), and undoes cleanly. Uses a
// RECTANGULAR prism so a 90 twist is unambiguous - the top footprint swaps.

#include "core/Document.h"
#include "modeling/MoveFaceOp.h"

#include <gtest/gtest.h>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <BRepGProp_Face.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <cmath>

namespace {
double vol(const TopoDS_Shape& s) {
    GProp_GProps g; BRepGProp::VolumeProperties(s, g); return g.Mass();
}
// The +Z-facing (top) face of a box.
TopoDS_Face topFace(const TopoDS_Shape& body) {
    for (TopExp_Explorer ex(body, TopAbs_FACE); ex.More(); ex.Next()) {
        TopoDS_Face f = TopoDS::Face(ex.Current());
        BRepGProp_Face p(f);
        double u1,u2,v1,v2; p.Bounds(u1,u2,v1,v2);
        gp_Pnt c; gp_Vec n; p.Normal(0.5*(u1+u2), 0.5*(v1+v2), c, n);
        if (n.Magnitude() > 1e-9 && n.Normalized().Z() > 0.99) return f;
    }
    return TopoDS_Face();
}
double PI() { return 3.14159265358979323846; }
}

TEST(TwistFace, Rectangle90SwapsFootprintValidVolumePreserved) {
    Document doc;
    // 10 (x) x 6 (y) x 10 (z) prism.
    TopoDS_Shape box = BRepPrimAPI_MakeBox(gp_Pnt(0,0,0), 10, 6, 10).Shape();
    int body = doc.addBody(box, "bar");
    const double v0 = vol(box);

    TopoDS_Face top = topFace(doc.getBody(body));
    ASSERT_FALSE(top.IsNull());

    MoveFaceOp op;
    op.setBody(body);
    op.setFace(top);
    op.setKind(MoveFaceOp::Kind::Twist);
    op.setTwist(PI() / 2.0); // 90 degrees
    ASSERT_TRUE(op.execute(doc));

    const TopoDS_Shape& r = doc.getBody(body);
    EXPECT_TRUE(BRepCheck_Analyzer(r).IsValid()) << "twisted solid must be valid";
    // Near-preserving: a ruled (faceted) twist chords the helicoid so it shaves
    // a sliver - within ~2% and never MORE than the prism.
    EXPECT_LT(vol(r), v0 + 1e-6) << "ruled twist can't add volume";
    EXPECT_GT(vol(r), v0 * 0.98) << "twist shouldn't lose much volume";

    // The overall bbox now needs room for the diagonal of the swept rectangle,
    // so it grows in both X and Y vs the untwisted 10x6 - a plain (non-twisting)
    // loft would keep 10x6. Confirms a genuine twist happened.
    Bnd_Box bb; BRepBndLib::Add(r, bb);
    double x0,y0,z0,x1,y1,z1; bb.Get(x0,y0,z0,x1,y1,z1);
    EXPECT_GT(y1 - y0, 6.5) << "twist widens the Y extent past the base 6mm";
    EXPECT_NEAR(z1 - z0, 10.0, 1e-6) << "height unchanged";
}

TEST(TwistFace, UndoRestoresOriginal) {
    Document doc;
    TopoDS_Shape box = BRepPrimAPI_MakeBox(gp_Pnt(0,0,0), 8, 8, 12).Shape();
    int body = doc.addBody(box, "post");
    const double v0 = vol(box);

    MoveFaceOp op;
    op.setBody(body);
    op.setFace(topFace(doc.getBody(body)));
    op.setKind(MoveFaceOp::Kind::Twist);
    op.setTwist(PI() / 3.0); // 60 degrees
    ASSERT_TRUE(op.execute(doc));
    EXPECT_TRUE(BRepCheck_Analyzer(doc.getBody(body)).IsValid());

    ASSERT_TRUE(op.undo(doc));
    EXPECT_NEAR(vol(doc.getBody(body)), v0, 1e-6) << "undo restores the prism";
}

TEST(TwistFace, ZeroAngleRefused) {
    Document doc;
    TopoDS_Shape box = BRepPrimAPI_MakeBox(gp_Pnt(0,0,0), 5, 5, 5).Shape();
    int body = doc.addBody(box, "cube");
    MoveFaceOp op;
    op.setBody(body);
    op.setFace(topFace(doc.getBody(body)));
    op.setKind(MoveFaceOp::Kind::Twist);
    op.setTwist(0.0);
    EXPECT_FALSE(op.execute(doc)) << "a zero twist is a no-op, refuse it";
}
