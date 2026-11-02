// ShellOp with zero openings (a sealed hollow) and with several (a tube).
// MakeThickSolid can't build the sealed case itself - an empty closing list
// returns only the shrunk solid - so ShellOp assembles it; these pin that.

#include "core/Document.h"
#include "modeling/ShellOp.h"
#include "modeling/FilletOp.h"

#include <gtest/gtest.h>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepGProp_Face.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <TopExp_Explorer.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <gp_Vec.hxx>
#include <chrono>
#include <cmath>
#include <vector>

namespace {

double vol(const TopoDS_Shape& s) { GProp_GProps g; BRepGProp::VolumeProperties(s, g); return g.Mass(); }
int shellCount(const TopoDS_Shape& s) {
    TopTools_IndexedMapOfShape m; TopExp::MapShapes(s, TopAbs_SHELL, m); return m.Extent();
}

TopoDS_Face faceFacing(const TopoDS_Shape& body, double nz) {
    for (TopExp_Explorer ex(body, TopAbs_FACE); ex.More(); ex.Next()) {
        TopoDS_Face f = TopoDS::Face(ex.Current());
        BRepGProp_Face gf(f);
        Standard_Real u0, u1, v0, v1; gf.Bounds(u0, u1, v0, v1);
        gp_Pnt p; gp_Vec n; gf.Normal((u0 + u1) / 2, (v0 + v1) / 2, p, n);
        n.Normalize();
        if (std::abs(n.Z() - nz) < 1e-6) return f;
    }
    return {};
}

int addBox(Document& doc) {
    return doc.addBody(BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 0), 20, 20, 20).Shape(), "box");
}

int addFilletedBox(Document& doc) {
    TopoDS_Shape box = BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 0), 20, 20, 20).Shape();
    int id = doc.addBody(box, "fbox");
    std::vector<TopoDS_Edge> verts;
    for (TopExp_Explorer ex(box, TopAbs_EDGE); ex.More(); ex.Next()) {
        BRepAdaptor_Curve c(TopoDS::Edge(ex.Current()));
        if (c.GetType() == GeomAbs_Line &&
            std::abs(c.Line().Direction().Dot(gp_Dir(0, 0, 1))) > 0.999)
            verts.push_back(TopoDS::Edge(ex.Current()));
    }
    FilletOp f; f.setBody(id); f.setEdges(verts); f.setRadius(3.0);
    EXPECT_TRUE(f.execute(doc));
    return id;
}

}  // namespace

TEST(ShellSealed, BoxHasAnInternalVoid) {
    Document doc;
    int id = addBox(doc);
    ShellOp op; op.setBody(id); op.setThickness(2.0);   // no faces: sealed
    ASSERT_TRUE(op.execute(doc));
    const TopoDS_Shape s = doc.getBody(id);
    EXPECT_TRUE(BRepCheck_Analyzer(s).IsValid());
    EXPECT_EQ(shellCount(s), 2);                         // outer skin + void
    EXPECT_NEAR(vol(s), 8000.0 - 16.0 * 16.0 * 16.0, 1.0);
}

TEST(ShellSealed, UndoRestoresTheSolid) {
    Document doc;
    int id = addBox(doc);
    ShellOp op; op.setBody(id); op.setThickness(2.0);
    ASSERT_TRUE(op.execute(doc));
    ASSERT_TRUE(op.undo(doc));
    EXPECT_NEAR(vol(doc.getBody(id)), 8000.0, 1e-3);
}

TEST(ShellSealed, FilletedBodyKeepsItsRoundedCavity) {
    Document doc;
    int id = addFilletedBox(doc);
    const double solid = vol(doc.getBody(id));
    ShellOp op; op.setBody(id); op.setThickness(2.0);
    ASSERT_TRUE(op.execute(doc));
    const TopoDS_Shape s = doc.getBody(id);
    EXPECT_TRUE(BRepCheck_Analyzer(s).IsValid());
    EXPECT_EQ(shellCount(s), 2);
    EXPECT_GT(vol(s), 0.0);
    EXPECT_LT(vol(s), solid * 0.6);
}

TEST(ShellSealed, TooThickAWallFailsCleanlyAndFast) {
    Document doc;
    int id = addBox(doc);
    auto t0 = std::chrono::steady_clock::now();
    ShellOp op; op.setBody(id); op.setThickness(10.0);   // swallows a 20 mm cube
    EXPECT_FALSE(op.execute(doc));
    EXPECT_LT(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), 5.0);
    EXPECT_NEAR(vol(doc.getBody(id)), 8000.0, 1e-3);     // body untouched
}

TEST(ShellSealed, WallAtAConcaveFilletRadiusDoesNotHang) {
    Document doc;
    int id = addFilletedBox(doc);
    auto t0 = std::chrono::steady_clock::now();
    ShellOp op; op.setBody(id); op.setThickness(3.5);    // past the R3 fillet
    (void)op.execute(doc);                               // either answer is fine; hanging is not
    EXPECT_LT(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), 10.0);
}

TEST(ShellMulti, TwoOpeningsMakeATube) {
    Document doc;
    int id = addBox(doc);
    TopoDS_Shape box = doc.getBody(id);
    ShellOp op; op.setBody(id); op.setThickness(2.0);
    op.addFaceToRemove(faceFacing(box, 1.0));
    op.addFaceToRemove(faceFacing(box, -1.0));
    ASSERT_TRUE(op.execute(doc));
    const TopoDS_Shape s = doc.getBody(id);
    EXPECT_TRUE(BRepCheck_Analyzer(s).IsValid());
    EXPECT_EQ(shellCount(s), 1);
    EXPECT_NEAR(vol(s), 2880.0, 1.0);   // 20^3 - 16*16*20 open both ends = 8000-5120
}

TEST(ShellSealed, ZeroOpeningsSerialiseWithoutFaces) {
    Document doc;
    int id = addBox(doc);
    ShellOp op; op.setBody(id); op.setThickness(2.0);
    ASSERT_TRUE(op.execute(doc));
    const std::string blob = op.serializeParams();
    EXPECT_EQ(blob.find("faces="), std::string::npos);
    ShellOp back;
    ASSERT_TRUE(back.deserializeParams(blob));
    EXPECT_EQ(back.getBodyId(), id);
    EXPECT_NEAR(back.getThickness(), 2.0, 1e-6);
}

// The GUI hands the op faces picked on the SEALED PREVIEW body, not on the
// original it is applied to. Every outer face of the sealed result must map
// back onto its original face.
TEST(ShellMulti, FacesPickedOnTheSealedPreviewMapBack) {
    for (double nz : {1.0, -1.0}) {
        Document doc;
        int id = addBox(doc);
        TopoDS_Shape original = doc.getBody(id);
        Document previewDoc;
        int pid = previewDoc.addBody(original, "p");
        ShellOp sealed; sealed.setBody(pid); sealed.setThickness(1.0);
        ASSERT_TRUE(sealed.execute(previewDoc));
        // The OUTER face with this normal: the one on the original boundary.
        TopoDS_Face picked;
        for (TopExp_Explorer ex(previewDoc.getBody(pid), TopAbs_FACE); ex.More(); ex.Next()) {
            TopoDS_Face f = TopoDS::Face(ex.Current());
            BRepGProp_Face gf(f);
            Standard_Real u0, u1, v0, v1; gf.Bounds(u0, u1, v0, v1);
            gp_Pnt p; gp_Vec n; gf.Normal((u0 + u1) / 2, (v0 + v1) / 2, p, n);
            n.Normalize();
            const double onBoundary = nz > 0 ? 20.0 : 0.0;
            if (std::abs(n.Z() - nz) < 1e-6 && std::abs(p.Z() - onBoundary) < 1e-6) picked = f;
        }
        ASSERT_FALSE(picked.IsNull());
        ShellOp op; op.setBody(id); op.setThickness(1.0); op.addFaceToRemove(picked);
        EXPECT_TRUE(op.execute(doc)) << "nz=" << nz;
        // Open cup, 1 mm walls.
        EXPECT_NEAR(vol(doc.getBody(id)), 8000.0 - 18.0 * 18.0 * 19.0, 1.0) << "nz=" << nz;
    }
}
