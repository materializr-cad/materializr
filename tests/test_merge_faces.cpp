// Merge Faces (#81): the repair for geometry that arrives already split.
//
// Two scopes with deliberately different appetites, and the tests pin the
// difference:
//
//   * whole body - only merges what is EXACTLY coplanar, so it is safe to point
//     at a whole part.
//   * picked faces - bounded to the edges between the picked faces, so it may
//     escalate the angular tolerance. It must still refuse a real corner.
//
// The escalation itself was measured on the reporting user's imported nacelle,
// where all 41 surviving seams sat between planes 1e-4 and 1e-2 rad apart -
// nothing a body-wide tolerance can ever safely reach. These tests cover the
// contract around that; the tolerance ladder's numbers live in MergeFacesOp.cpp.
#include <gtest/gtest.h>

#include "core/Document.h"
#include "modeling/MergeFacesOp.h"

#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <GProp_GProps.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <BRepGProp_Face.hxx>
#include <gp_Pnt.hxx>
#include <gp_Vec.hxx>

#include <cmath>
#include <vector>

namespace {

int faceCount(const TopoDS_Shape& s) {
    TopTools_IndexedMapOfShape m;
    TopExp::MapShapes(s, TopAbs_FACE, m);
    return m.Extent();
}

double volumeOf(const TopoDS_Shape& s) {
    GProp_GProps g;
    BRepGProp::VolumeProperties(s, g);
    return g.Mass();
}

bool faceNormalPoint(const TopoDS_Shape& f, gp_Dir& n, gp_Pnt& p) {
    BRepGProp_Face gf(TopoDS::Face(f));
    Standard_Real u0, u1, v0, v1; gf.Bounds(u0, u1, v0, v1);
    gp_Vec vn;
    gf.Normal(0.5 * (u0 + u1), 0.5 * (v0 + v1), p, vn);
    if (vn.Magnitude() < 1e-9) return false;
    n = gp_Dir(vn);
    return true;
}

// Every face whose outward normal points along +dir.
std::vector<TopoDS_Shape> facesFacing(const TopoDS_Shape& body, const gp_Dir& dir) {
    std::vector<TopoDS_Shape> out;
    for (TopExp_Explorer ex(body, TopAbs_FACE); ex.More(); ex.Next()) {
        gp_Dir n; gp_Pnt p;
        if (faceNormalPoint(ex.Current(), n, p) && n.Dot(dir) > 0.999)
            out.push_back(ex.Current());
    }
    return out;
}

// Two boxes fused side by side with the RAW kernel call - no unify - so the
// shared top plane comes back as two coplanar faces with a seam between them.
// That is the shape of the bug: geometrically one surface, topologically two.
TopoDS_Shape splitTopBar() {
    const TopoDS_Shape a = BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 0), 10, 10, 10).Shape();
    const TopoDS_Shape b = BRepPrimAPI_MakeBox(gp_Pnt(10, 0, 0), 10, 10, 10).Shape();
    BRepAlgoAPI_Fuse fuse(a, b);
    fuse.Build();
    return fuse.Shape();
}

} // namespace

TEST(MergeFaces, WholeBodyRefusesOnCleanGeometry) {
    Document doc;
    const int id = doc.addBody(BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape(), "Box");

    MergeFacesOp op;
    op.setBody(id);
    // A pristine box has nothing to merge. Refusing is what keeps History from
    // adding a step that did nothing.
    EXPECT_FALSE(op.execute(doc));
    EXPECT_EQ(faceCount(doc.getBody(id)), 6);
}

TEST(MergeFaces, WholeBodyMergesExactlyCoplanarSeams) {
    Document doc;
    const TopoDS_Shape bar = splitTopBar();
    ASSERT_FALSE(bar.IsNull());
    const int id = doc.addBody(bar, "Bar");
    const int before = faceCount(bar);
    const double vol = volumeOf(bar);
    ASSERT_GT(before, 6);   // the raw fuse really did leave the seams

    MergeFacesOp op;
    op.setBody(id);
    ASSERT_TRUE(op.execute(doc));

    const TopoDS_Shape after = doc.getBody(id);
    EXPECT_LT(faceCount(after), before);
    EXPECT_EQ(faceCount(after), 6);                     // back to a plain box
    EXPECT_NEAR(volumeOf(after), vol, 1e-4 * vol);      // and it did not reshape
}

TEST(MergeFaces, FaceScopedMergesThePickedPair) {
    Document doc;
    const int id = doc.addBody(splitTopBar(), "Bar");
    const TopoDS_Shape body = doc.getBody(id);
    const int before = faceCount(body);

    const std::vector<TopoDS_Shape> tops = facesFacing(body, gp_Dir(0, 0, 1));
    ASSERT_EQ(tops.size(), 2u);   // the split top

    MergeFacesOp op;
    op.setBody(id);
    op.setFaces(tops);
    EXPECT_TRUE(op.isFaceScoped());
    ASSERT_TRUE(op.execute(doc));

    const TopoDS_Shape after = doc.getBody(id);
    EXPECT_LT(faceCount(after), before);
    EXPECT_EQ(facesFacing(after, gp_Dir(0, 0, 1)).size(), 1u);
}

TEST(MergeFaces, FaceScopedRefusesARealCorner) {
    Document doc;
    const int id = doc.addBody(BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape(), "Box");
    const TopoDS_Shape body = doc.getBody(id);

    const auto top = facesFacing(body, gp_Dir(0, 0, 1));
    const auto side = facesFacing(body, gp_Dir(1, 0, 0));
    ASSERT_EQ(top.size(), 1u);
    ASSERT_EQ(side.size(), 1u);

    MergeFacesOp op;
    op.setBody(id);
    op.setFaces({top[0], side[0]});
    // Two faces at 90 degrees are not one surface at any tolerance this op is
    // willing to reach. Loosening the ladder until this passes would mean the
    // op can flatten a part.
    EXPECT_FALSE(op.execute(doc));
    EXPECT_EQ(faceCount(doc.getBody(id)), 6);
}

TEST(MergeFaces, FaceScopedRefusesASingleFace) {
    Document doc;
    const int id = doc.addBody(splitTopBar(), "Bar");
    const auto tops = facesFacing(doc.getBody(id), gp_Dir(0, 0, 1));
    ASSERT_FALSE(tops.empty());

    MergeFacesOp op;
    op.setBody(id);
    op.setFaces({tops[0]});
    EXPECT_FALSE(op.execute(doc));
}

TEST(MergeFaces, UndoRestoresTheSplitFaces) {
    Document doc;
    const int id = doc.addBody(splitTopBar(), "Bar");
    const int before = faceCount(doc.getBody(id));
    const double vol = volumeOf(doc.getBody(id));

    MergeFacesOp op;
    op.setBody(id);
    op.setFaces(facesFacing(doc.getBody(id), gp_Dir(0, 0, 1)));
    ASSERT_TRUE(op.execute(doc));
    ASSERT_LT(faceCount(doc.getBody(id)), before);

    ASSERT_TRUE(op.undo(doc));
    EXPECT_EQ(faceCount(doc.getBody(id)), before);
    EXPECT_NEAR(volumeOf(doc.getBody(id)), vol, 1e-9 * vol);
}

TEST(MergeFaces, AnchorsSurviveSerialisation) {
    Document doc;
    const int id = doc.addBody(splitTopBar(), "Bar");

    MergeFacesOp op;
    op.setBody(id);
    op.setFaces(facesFacing(doc.getBody(id), gp_Dir(0, 0, 1)));
    ASSERT_TRUE(op.execute(doc));

    // The picked faces belong to a session that will not exist on reload, so
    // the op persists anchors instead. A round-trip has to come back
    // face-scoped, or a replay would silently widen into a whole-body merge.
    const std::string blob = op.serializeParams();
    MergeFacesOp reloaded;
    ASSERT_TRUE(reloaded.deserializeParams(blob));
    EXPECT_EQ(reloaded.getBodyId(), id);
    EXPECT_TRUE(reloaded.isFaceScoped());
    EXPECT_EQ(reloaded.getFacesBefore(), op.getFacesBefore());
    EXPECT_EQ(reloaded.getFacesAfter(), op.getFacesAfter());
}
