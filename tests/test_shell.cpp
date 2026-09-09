// Regression tests for ShellOp on a FILLETED body - the covus-nose failure:
//  1. An over-thick wall (>= a concave fillet radius) must fail CLEANLY and
//     FAST. Previously the arc-join threw and the intersection-join fallback
//     spun in an unbounded internal loop ("Cote PT2PT3 nul"), freezing the app.
//  2. The opened face must survive a body REGENERATION: ShellOp stores a raw
//     TopoDS_Face, which goes stale when an upstream sketch edit rebuilds the
//     body; execute() now rebinds it geometrically, so the shell reproduces
//     instead of vanishing.

#include "core/Document.h"
#include "modeling/ShellOp.h"
#include "modeling/FilletOp.h"

#include <gtest/gtest.h>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepGProp_Face.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <TopExp_Explorer.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>
#include <chrono>
#include <vector>

namespace {

double vol(const TopoDS_Shape& s) {
    GProp_GProps g; BRepGProp::VolumeProperties(s, g); return g.Mass();
}

// The face whose outward normal points most in -Z (the box's bottom).
TopoDS_Face bottomFace(const TopoDS_Shape& body) {
    TopoDS_Face best; double bestDot = 1e9;
    for (TopExp_Explorer ex(body, TopAbs_FACE); ex.More(); ex.Next()) {
        TopoDS_Face f = TopoDS::Face(ex.Current());
        BRepGProp_Face gf(f);
        Standard_Real u0, u1, v0, v1; gf.Bounds(u0, u1, v0, v1);
        gp_Pnt p; gp_Vec n; gf.Normal((u0+u1)/2, (v0+v1)/2, p, n);
        if (n.Magnitude() < 1e-9) continue;
        n.Normalize();
        if (n.Z() < bestDot) { bestDot = n.Z(); best = f; }
    }
    return best;
}

// A 20mm box with its four vertical edges filleted R3.
TopoDS_Shape filletedBox(Document& doc, int& bodyId) {
    TopoDS_Shape box = BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 0), 20, 20, 20).Shape();
    bodyId = doc.addBody(box, "box");
    std::vector<TopoDS_Edge> verts;
    for (TopExp_Explorer ex(box, TopAbs_EDGE); ex.More(); ex.Next()) {
        BRepAdaptor_Curve c(TopoDS::Edge(ex.Current()));
        if (c.GetType() == GeomAbs_Line &&
            std::abs(c.Line().Direction().Dot(gp_Dir(0, 0, 1))) > 0.999)
            verts.push_back(TopoDS::Edge(ex.Current()));
    }
    FilletOp f; f.setBody(bodyId); f.setEdges(verts); f.setRadius(3.0);
    f.execute(doc);
    return doc.getBody(bodyId);
}

// A 20mm box with ALL 12 edges filleted to radius R - the opened face ends up
// ringed by fillets on every side, the case that used to no-op silently (#30).
TopoDS_Shape allFilletedBox(Document& doc, int& bodyId, double R) {
    TopoDS_Shape box = BRepPrimAPI_MakeBox(gp_Pnt(0, 0, 0), 20, 20, 20).Shape();
    bodyId = doc.addBody(box, "box");
    std::vector<TopoDS_Edge> edges;
    for (TopExp_Explorer ex(box, TopAbs_EDGE); ex.More(); ex.Next())
        edges.push_back(TopoDS::Edge(ex.Current()));
    FilletOp f; f.setBody(bodyId); f.setEdges(edges); f.setRadius(R);
    f.execute(doc);
    return doc.getBody(bodyId);
}

} // namespace

// An over-thick wall fails cleanly and quickly - never hangs.
TEST(Shell, OverThickWallFailsFastNotHang) {
    Document doc;
    int body; TopoDS_Shape solid = filletedBox(doc, body);
    TopoDS_Face bf = bottomFace(solid);
    ASSERT_FALSE(bf.IsNull());

    ShellOp op;
    op.setBody(body);
    op.setThickness(12.0); // > half the 20mm box: no valid inner wall
    op.addFaceToRemove(bf);

    auto t0 = std::chrono::steady_clock::now();
    bool ok = op.execute(doc);
    double sec = std::chrono::duration<double>(
                     std::chrono::steady_clock::now() - t0).count();

    EXPECT_FALSE(ok) << "an impossible wall thickness must fail";
    EXPECT_LT(sec, 10.0) << "must fail FAST - a multi-second run means the "
                            "intersection-join hang wasn't avoided";
    // The body must be untouched on failure (execute must not commit garbage).
    EXPECT_NEAR(vol(doc.getBody(body)), vol(solid), 1e-6);
}

// A reasonable wall shells fine.
TEST(Shell, ReasonableWallShells) {
    Document doc;
    int body; TopoDS_Shape solid = filletedBox(doc, body);
    ShellOp op;
    op.setBody(body);
    op.setThickness(1.5);
    op.addFaceToRemove(bottomFace(solid));
    ASSERT_TRUE(op.execute(doc));
    EXPECT_LT(vol(doc.getBody(body)), vol(solid)) << "shell removes material";
    EXPECT_GT(vol(doc.getBody(body)), 0.0);
}

// A face ringed by fillets on every edge CANNOT be opened by OCCT's offset -
// it seals the cavity into a closed void (2 shells) instead of an open cup, or
// no-ops outright. Both disguise as "valid" with reduced volume, so ShellOp
// insists on a single-shell OPEN cup and otherwise fails HONESTLY, leaving the
// body untouched (never a silent do-nothing that looks like the solid) (#30).
TEST(Shell, FilletRingedFaceFailsNotSilentSeal) {
    Document doc;
    int body; TopoDS_Shape solid = allFilletedBox(doc, body, 2.0);
    double before = vol(solid);
    ShellOp op;
    op.setBody(body);
    op.setThickness(1.0);            // well under the 2mm fillet radius
    op.addFaceToRemove(bottomFace(solid));
    EXPECT_FALSE(op.execute(doc)) << "a fillet-ringed face can't be opened";
    EXPECT_NEAR(vol(doc.getBody(body)), before, 1e-6) << "body untouched on fail";
}

// Same story for an over-radius wall: honest failure, body untouched.
TEST(Shell, AllEdgesFilletedOverRadiusFailsNotNoOp) {
    Document doc;
    int body; TopoDS_Shape solid = allFilletedBox(doc, body, 2.0);
    double before = vol(solid);
    ShellOp op;
    op.setBody(body);
    op.setThickness(2.5);            // over the 2mm fillet radius
    op.addFaceToRemove(bottomFace(solid));
    EXPECT_FALSE(op.execute(doc)) << "an un-shellable wall must fail, not no-op";
    EXPECT_NEAR(vol(doc.getBody(body)), before, 1e-6) << "body untouched on fail";
}

// Steve's #30 repro: fillet ONE face's 4 edges, then shell THAT face. OCCT
// cannot open a fillet-bordered face (it seals or no-ops); ShellOp must refuse
// rather than commit a sealed hollow that looks identical to the solid. The
// workaround is order-of-operations: shell first, fillet after.
TEST(Shell, FilletedFaceThenShellThatFace_FailsCleanly) {
    auto plusXFace = [](const TopoDS_Shape& b) {
        for (TopExp_Explorer ex(b, TopAbs_FACE); ex.More(); ex.Next()) {
            TopoDS_Face f = TopoDS::Face(ex.Current());
            BRepAdaptor_Surface sa(f);
            if (sa.GetType() != GeomAbs_Plane) continue;
            BRepGProp_Face gf(f);
            Standard_Real u0,u1,v0,v1; gf.Bounds(u0,u1,v0,v1);
            gp_Pnt p; gp_Vec n; gf.Normal((u0+u1)/2,(v0+v1)/2,p,n);
            if (n.Magnitude()<1e-9) continue; n.Normalize();
            if (n.X() > 0.9) return f;
        }
        return TopoDS_Face();
    };
    Document doc;
    TopoDS_Shape box = BRepPrimAPI_MakeBox(gp_Pnt(0,0,0), 20, 20, 20).Shape();
    int body = doc.addBody(box, "box");
    TopoDS_Face pf = plusXFace(box);
    std::vector<TopoDS_Edge> es;
    for (TopExp_Explorer ex(pf, TopAbs_EDGE); ex.More(); ex.Next())
        es.push_back(TopoDS::Edge(ex.Current()));
    FilletOp f; f.setBody(body); f.setEdges(es); f.setRadius(2.0); f.execute(doc);
    TopoDS_Shape solid = doc.getBody(body);
    double before = vol(solid);

    ShellOp op;
    op.setBody(body);
    op.setThickness(1.0);
    op.addFaceToRemove(plusXFace(solid));
    EXPECT_FALSE(op.execute(doc)) << "OCCT can't open a fillet-bordered face";
    EXPECT_NEAR(vol(doc.getBody(body)), before, 1e-6) << "body untouched on fail";
}

// Sanity: shelling a fillet-FREE face still produces a real OPEN cup (1 shell).
TEST(Shell, PlainFaceStillOpensToOneShell) {
    Document doc;
    TopoDS_Shape box = BRepPrimAPI_MakeBox(gp_Pnt(0,0,0), 20, 20, 20).Shape();
    int body = doc.addBody(box, "box");
    ShellOp op;
    op.setBody(body);
    op.setThickness(1.0);
    op.addFaceToRemove(bottomFace(box));
    ASSERT_TRUE(op.execute(doc)) << "a plain face must still shell open";
    TopoDS_Shape r = doc.getBody(body);
    EXPECT_LT(vol(r), vol(box) * 0.5) << "a real hollow";
    TopTools_IndexedMapOfShape shells;
    TopExp::MapShapes(r, TopAbs_SHELL, shells);
    EXPECT_EQ(shells.Extent(), 1) << "an open cup is a single shell, not a sealed void";
}

// The rounded-face radius detector finds the R3 fillet, so the panel can name
// it when a near-radius wall fails.
TEST(Shell, RoundedFaceRadiiFindsFillet) {
    Document doc;
    int body; TopoDS_Shape solid = filletedBox(doc, body);
    auto radii = ShellOp::roundedFaceRadii(solid);
    ASSERT_FALSE(radii.empty()) << "the R3 fillet faces must be detected";
    bool has3 = false;
    for (double r : radii) if (std::abs(r - 3.0) < 1e-3) has3 = true;
    EXPECT_TRUE(has3) << "detector must report the 3mm fillet radius";
}

// The opened face rebinds after the body is regenerated (moved), so a shell
// re-executed by the history cascade reproduces instead of vanishing.
TEST(Shell, OpenFaceRebindsAfterBodyRegenerated) {
    Document doc;
    int body; TopoDS_Shape solid = filletedBox(doc, body);
    ShellOp op;
    op.setBody(body);
    op.setThickness(1.5);
    op.addFaceToRemove(bottomFace(solid));
    ASSERT_TRUE(op.execute(doc));
    const double shelledVol = vol(doc.getBody(body));

    // Simulate an upstream regeneration: replace the body with a moved copy of
    // the ORIGINAL solid (fresh TopoDS_Face handles), then re-run the SAME op.
    gp_Trsf tr; tr.SetTranslation(gp_Vec(0, 0, 40));
    doc.updateBody(body, BRepBuilderAPI_Transform(solid, tr, true).Shape());

    ASSERT_TRUE(op.execute(doc))
        << "the opened face must rebind to the regenerated body, not be lost";
    EXPECT_NEAR(vol(doc.getBody(body)), shelledVol, 1e-3)
        << "the re-derived shell should match the original";
}
