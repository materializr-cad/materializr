// Local face tweak: move a face, rebuild only what touches it.
//
// The whole point is what does NOT move. MoveFaceOp shears the entire body
// through a GTransform, so sliding a box's top sideways drags every interior
// feature with it; this rebuilds three vertices and leaves the rest of the
// solid byte-for-byte where it was. The tests below are written around that
// distinction — volume and validity say the rebuild is sound, but the
// assertions about untouched geometry are what say it is LOCAL.
#include <gtest/gtest.h>

#include "modeling/FaceTweak.h"

#include <BRepAdaptor_Surface.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <gp_Ax1.hxx>
#include <gp_Ax2.hxx>
#include <gp_Trsf.hxx>

#include <cmath>

using namespace materializr;

namespace {

double vol(const TopoDS_Shape& s) {
    GProp_GProps g;
    BRepGProp::VolumeProperties(s, g);
    return g.Mass();
}

int faceCount(const TopoDS_Shape& s) {
    int n = 0;
    for (TopExp_Explorer ex(s, TopAbs_FACE); ex.More(); ex.Next()) ++n;
    return n;
}

// The face of `s` whose centre sits furthest along `dir`.
TopoDS_Face faceTowards(const TopoDS_Shape& s, const gp_Dir& dir) {
    TopoDS_Face best;
    double bestD = -1e30;
    for (TopExp_Explorer ex(s, TopAbs_FACE); ex.More(); ex.Next()) {
        GProp_GProps g;
        BRepGProp::SurfaceProperties(ex.Current(), g);
        const gp_Pnt c = g.CentreOfMass();
        const double d = c.X() * dir.X() + c.Y() * dir.Y() + c.Z() * dir.Z();
        if (d > bestD) { bestD = d; best = TopoDS::Face(ex.Current()); }
    }
    return best;
}

// Lowest and highest Z of any vertex, so a test can say which end moved.
void zRange(const TopoDS_Shape& s, double& lo, double& hi) {
    lo = 1e30; hi = -1e30;
    for (TopExp_Explorer ex(s, TopAbs_VERTEX); ex.More(); ex.Next()) {
        const double z = BRep_Tool::Pnt(TopoDS::Vertex(ex.Current())).Z();
        lo = std::min(lo, z);
        hi = std::max(hi, z);
    }
}

} // namespace

// ── The move that PushPull already does, done locally ───────────────────────

TEST(FaceTweak, OffsetAlongTheNormalChangesOnlyThatEnd) {
    const TopoDS_Shape box = BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape();
    gp_Trsf up;
    up.SetTranslation(gp_Vec(0, 0, 4));

    const auto r = tweak::moveFace(box, faceTowards(box, gp_Dir(0, 0, 1)), up);
    ASSERT_TRUE(r.ok()) << tweak::refusalText(r.refusal);
    EXPECT_NEAR(vol(r.shape), 10.0 * 10.0 * 14.0, 1e-6);
    EXPECT_EQ(faceCount(r.shape), 6);

    double lo, hi;
    zRange(r.shape, lo, hi);
    EXPECT_NEAR(lo, 0.0, 1e-9) << "the bottom must not have moved";
    EXPECT_NEAR(hi, 14.0, 1e-9);
}

// ── The gesture that has no local answer, and why ──────────────────────────

TEST(FaceTweak, SlidingAFlatFaceInsideItsOwnPlaneIsRefused) {
    // Translating a plane along itself lands on the same plane, so every corner
    // re-solves exactly where it started. This is not a gap in the engine, it is
    // what the geometry says — and it is the reason MoveFaceOp exists to answer
    // the same gesture by shearing the whole body instead. Reporting it beats
    // returning an identical solid and calling it a success.
    const TopoDS_Shape box = BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape();
    gp_Trsf slide;
    slide.SetTranslation(gp_Vec(3, 0, 0));

    const auto r = tweak::moveFace(box, faceTowards(box, gp_Dir(0, 0, 1)), slide);
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.refusal, tweak::Refusal::NoChange);
}

TEST(FaceTweak, SlideCombinedWithAnOffsetIsJustTheOffset) {
    // The in-plane half contributes nothing, so this must behave exactly like
    // the pure offset — same volume, same untouched base.
    const TopoDS_Shape box = BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape();
    gp_Trsf mixed;
    mixed.SetTranslation(gp_Vec(3, 0, 4));

    const auto r = tweak::moveFace(box, faceTowards(box, gp_Dir(0, 0, 1)), mixed);
    ASSERT_TRUE(r.ok()) << tweak::refusalText(r.refusal);
    EXPECT_NEAR(vol(r.shape), 1400.0, 1e-6);
    double lo, hi;
    zRange(r.shape, lo, hi);
    EXPECT_NEAR(lo, 0.0, 1e-9);
    EXPECT_NEAR(hi, 14.0, 1e-9);
}

TEST(FaceTweak, TiltRotatesTheFaceAboutItsOwnEdge) {
    const TopoDS_Shape box = BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape();
    // Tip the top face 10 degrees about the y axis through one of its edges.
    gp_Trsf tilt;
    tilt.SetRotation(gp_Ax1(gp_Pnt(0, 0, 10), gp_Dir(0, 1, 0)), 10.0 * M_PI / 180.0);

    const auto r = tweak::moveFace(box, faceTowards(box, gp_Dir(0, 0, 1)), tilt);
    ASSERT_TRUE(r.ok()) << tweak::refusalText(r.refusal);
    EXPECT_EQ(faceCount(r.shape), 6);
    EXPECT_TRUE(BRepCheck_Analyzer(r.shape).IsValid());

    double lo, hi;
    zRange(r.shape, lo, hi);
    EXPECT_NEAR(lo, 0.0, 1e-9) << "the base is untouched by a tilt of the top";
    // The far edge drops by 10*tan(10deg); the pivot edge stays at z=10.
    EXPECT_NEAR(hi, 10.0, 1e-6);
    // A wedge removed from a 1000 box: 0.5 * 10 * (10*tan10) * 10.
    const double wedge = 0.5 * 10.0 * (10.0 * std::tan(10.0 * M_PI / 180.0)) * 10.0;
    EXPECT_NEAR(vol(r.shape), 1000.0 - wedge, 1e-3);
}

// ── Refusals are explicit, never a mangled body ─────────────────────────────

TEST(FaceTweak, CurvedFaceIsRefusedNotAttempted) {
    const TopoDS_Shape cyl = BRepPrimAPI_MakeCylinder(5.0, 10.0).Shape();
    TopoDS_Face wall;
    for (TopExp_Explorer ex(cyl, TopAbs_FACE); ex.More(); ex.Next()) {
        BRepAdaptor_Surface s(TopoDS::Face(ex.Current()));
        if (s.GetType() == GeomAbs_Cylinder) { wall = TopoDS::Face(ex.Current()); break; }
    }
    ASSERT_FALSE(wall.IsNull());
    gp_Trsf t;
    t.SetTranslation(gp_Vec(1, 0, 0));
    const auto r = tweak::moveFace(cyl, wall, t);
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.refusal, tweak::Refusal::NotPlanar);
    EXPECT_STRNE(tweak::refusalText(r.refusal), "");
}

TEST(FaceTweak, FlatTopOfACylinderOffsetsAlongItsWall) {
    // The lid of a cylinder: planar itself, with nothing but the curved wall
    // meeting it. There is no third plane at any of its corners, which is why
    // the three-plane version of this engine had to refuse it outright. Solving
    // the corner off the LEAVING edge instead needs no such thing — the seam
    // runs up the wall and simply crosses the new plane higher up.
    const TopoDS_Shape cyl = BRepPrimAPI_MakeCylinder(5.0, 10.0).Shape();
    gp_Trsf up;
    up.SetTranslation(gp_Vec(0, 0, 3));

    const auto r = tweak::moveFace(cyl, faceTowards(cyl, gp_Dir(0, 0, 1)), up);
    ASSERT_TRUE(r.ok()) << tweak::refusalText(r.refusal);
    EXPECT_NEAR(vol(r.shape), M_PI * 25.0 * 13.0, 1e-6);
    EXPECT_EQ(faceCount(r.shape), 3);
    EXPECT_TRUE(BRepCheck_Analyzer(r.shape).IsValid());

    double lo, hi;
    zRange(r.shape, lo, hi);
    EXPECT_NEAR(lo, 0.0, 1e-9) << "the base is untouched";
    EXPECT_NEAR(hi, 13.0, 1e-9);
}

TEST(FaceTweak, TiltingACylinderLidCutsItToAnEllipse) {
    // Tilt the lid about a diameter through its own centre. The wedge added on
    // one side is the mirror of the wedge removed on the other, so the volume
    // is unchanged — and the lid is no longer a circle but the ellipse where
    // the tilted plane crosses the wall. Nothing about that is expressible with
    // planes, which makes it the test that says curved neighbours work.
    const double r = 5.0, h = 10.0;
    const TopoDS_Shape cyl = BRepPrimAPI_MakeCylinder(r, h).Shape();
    gp_Trsf tilt;
    tilt.SetRotation(gp_Ax1(gp_Pnt(0, 0, h), gp_Dir(0, 1, 0)), 10.0 * M_PI / 180.0);

    const auto res = tweak::moveFace(cyl, faceTowards(cyl, gp_Dir(0, 0, 1)), tilt);
    ASSERT_TRUE(res.ok()) << tweak::refusalText(res.refusal);
    EXPECT_TRUE(BRepCheck_Analyzer(res.shape).IsValid());
    EXPECT_NEAR(vol(res.shape), M_PI * r * r * h, 1e-3)
        << "tilting about a centre diameter trades equal wedges";
    EXPECT_EQ(faceCount(res.shape), 3);

    // The lid: an ellipse with the same minor axis as the cylinder radius and a
    // major axis stretched by 1/cos(tilt). Its area is the giveaway.
    const TopoDS_Face lid = faceTowards(res.shape, gp_Dir(0, 0, 1));
    GProp_GProps g;
    BRepGProp::SurfaceProperties(lid, g);
    EXPECT_NEAR(g.Mass(), M_PI * r * r / std::cos(10.0 * M_PI / 180.0), 1e-3);
}

TEST(FaceTweak, TheBoreThroughAMovedFaceSurvivesIt) {
    // A box with a bore up through it, top face raised. Every corner of the
    // hole in that face is a SEAM vertex where only two faces meet — no third
    // surface to solve against — and the hole's rim is a circle, not a segment.
    // This is the case the planar version refused outright, and the one real
    // parts are full of.
    const TopoDS_Shape box =
        BRepPrimAPI_MakeBox(gp_Pnt(-10, -10, 0), 20.0, 20.0, 10.0).Shape();
    const TopoDS_Shape drill =
        BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(0, 0, -1), gp_Dir(0, 0, 1)), 3.0, 12.0).Shape();
    BRepAlgoAPI_Cut cut(box, drill);
    cut.Build();
    ASSERT_TRUE(cut.IsDone());
    const TopoDS_Shape bored = cut.Shape();
    const double before = vol(bored);

    gp_Trsf up;
    up.SetTranslation(gp_Vec(0, 0, 5));
    const auto r = tweak::moveFace(bored, faceTowards(bored, gp_Dir(0, 0, 1)), up);
    ASSERT_TRUE(r.ok()) << tweak::refusalText(r.refusal);
    EXPECT_TRUE(BRepCheck_Analyzer(r.shape).IsValid());

    // 5 mm more box, less the 5 mm of bore that went up with it.
    EXPECT_NEAR(vol(r.shape), before + 5.0 * (400.0 - M_PI * 9.0), 1e-3);

    // And the bore itself is where it always was.
    bool foundBore = false;
    for (TopExp_Explorer ex(r.shape, TopAbs_FACE); ex.More(); ex.Next()) {
        BRepAdaptor_Surface s(TopoDS::Face(ex.Current()));
        if (s.GetType() != GeomAbs_Cylinder) continue;
        foundBore = true;
        EXPECT_NEAR(s.Cylinder().Location().X(), 0.0, 1e-6);
        EXPECT_NEAR(s.Cylinder().Location().Y(), 0.0, 1e-6);
        EXPECT_NEAR(s.Cylinder().Radius(), 3.0, 1e-9);
    }
    EXPECT_TRUE(foundBore) << "the bore must still be a cylinder of its own";
}

TEST(FaceTweak, AFaceFromAnotherBodyIsRefused) {
    const TopoDS_Shape a = BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape();
    const TopoDS_Shape b = BRepPrimAPI_MakeBox(gp_Pnt(50, 0, 0), 10.0, 10.0, 10.0).Shape();
    gp_Trsf t;
    t.SetTranslation(gp_Vec(0, 0, 1));
    const auto r = tweak::moveFace(a, faceTowards(b, gp_Dir(0, 0, 1)), t);
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.refusal, tweak::Refusal::FaceNotFound);
}

TEST(FaceTweak, AMoveThatFlattensTheBodyIsRefused) {
    const TopoDS_Shape box = BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape();
    gp_Trsf down;
    down.SetTranslation(gp_Vec(0, 0, -10));   // top lands exactly on the bottom
    const auto r = tweak::moveFace(box, faceTowards(box, gp_Dir(0, 0, 1)), down);
    EXPECT_FALSE(r.ok()) << "a zero-volume solid is not a valid answer";
}

// ── The history step ────────────────────────────────────────────────────────

#include "core/Document.h"
#include "core/History.h"
#include "modeling/FaceTweakOp.h"
#include "modeling/OperationFactory.h"

namespace {
gp_Trsf tiltTop(double deg) {
    gp_Trsf t;
    t.SetRotation(gp_Ax1(gp_Pnt(0, 0, 10), gp_Dir(0, 1, 0)), deg * M_PI / 180.0);
    return t;
}
} // namespace

TEST(FaceTweakOp, AppliesAndUndoes) {
    Document doc;
    const TopoDS_Shape box = BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape();
    const int id = doc.addBody(box, "Box");

    FaceTweakOp op;
    op.setBody(id);
    op.setFace(faceTowards(box, gp_Dir(0, 0, 1)));
    op.setTransform(tiltTop(10.0));
    ASSERT_TRUE(op.execute(doc));

    const double wedge = 0.5 * 10.0 * (10.0 * std::tan(10.0 * M_PI / 180.0)) * 10.0;
    EXPECT_NEAR(vol(doc.getBody(id)), 1000.0 - wedge, 1e-3);

    const OperationDiff d = op.captureDiff();
    ASSERT_EQ(d.modifiedBefore.size(), 1u);
    EXPECT_EQ(d.modifiedBefore[0].first, id);

    ASSERT_TRUE(op.undo(doc));
    EXPECT_NEAR(vol(doc.getBody(id)), 1000.0, 1e-6);
}

TEST(FaceTweakOp, RefusalIsReportedNotSwallowed) {
    // The cylinder's curved WALL — the moved face itself has to be planar, and
    // that is still the real limit. (Its flat top is no longer a refusal: a
    // curved NEIGHBOUR is fine now.)
    Document doc;
    const TopoDS_Shape cyl = BRepPrimAPI_MakeCylinder(5.0, 10.0).Shape();
    const int id = doc.addBody(cyl, "Cyl");

    TopoDS_Face wall;
    for (TopExp_Explorer ex(cyl, TopAbs_FACE); ex.More(); ex.Next()) {
        BRepAdaptor_Surface s(TopoDS::Face(ex.Current()));
        if (s.GetType() == GeomAbs_Cylinder) { wall = TopoDS::Face(ex.Current()); break; }
    }
    ASSERT_FALSE(wall.IsNull());

    FaceTweakOp op;
    op.setBody(id);
    op.setFace(wall);
    gp_Trsf out;
    out.SetTranslation(gp_Vec(1, 0, 0));
    op.setTransform(out);
    EXPECT_FALSE(op.execute(doc));
    EXPECT_EQ(op.refusal(), tweak::Refusal::NotPlanar);
    // The body must be exactly as it was — a refused op leaves no residue.
    EXPECT_NEAR(vol(doc.getBody(id)), M_PI * 25.0 * 10.0, 1e-6);
}

TEST(FaceTweakOp, ParamsRoundTripAndReplay) {
    Document doc;
    const TopoDS_Shape box = BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape();
    const int id = doc.addBody(box, "Box");

    FaceTweakOp op;
    op.setBody(id);
    op.setFace(faceTowards(box, gp_Dir(0, 0, 1)));
    op.setTransform(tiltTop(10.0));
    ASSERT_TRUE(op.execute(doc));
    const double tilted = vol(doc.getBody(id));

    const std::string blob = op.serializeParams();
    auto back = OperationFactory::create("face_tweak");
    ASSERT_NE(back, nullptr) << "the factory has to know the type id";
    ASSERT_TRUE(back->deserializeParams(blob));

    // Replay onto a fresh document: same body, same result.
    Document doc2;
    const int id2 = doc2.addBody(box, "Box");
    ASSERT_EQ(id2, id);
    ASSERT_TRUE(back->execute(doc2));
    EXPECT_NEAR(vol(doc2.getBody(id2)), tilted, 1e-9);
}

TEST(FaceTweakOp, RebindsToARegeneratedBody) {
    // The stored TopoDS_Face is from the body as it stood when the gesture was
    // made. Replaying against a body rebuilt from scratch means every TShape is
    // new, so the op has to find its face by normal + centroid or the step is
    // dead on reload.
    Document doc;
    const TopoDS_Shape box = BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape();
    const int id = doc.addBody(box, "Box");
    FaceTweakOp op;
    op.setBody(id);
    op.setFace(faceTowards(box, gp_Dir(0, 0, 1)));
    op.setTransform(tiltTop(10.0));
    ASSERT_TRUE(op.execute(doc));
    const double tilted = vol(doc.getBody(id));

    // A structurally identical box built independently: same geometry, all-new
    // handles.
    doc.updateBody(id, BRepPrimAPI_MakeBox(10.0, 10.0, 10.0).Shape());
    ASSERT_TRUE(op.execute(doc)) << "rebind by normal + centroid";
    EXPECT_NEAR(vol(doc.getBody(id)), tilted, 1e-9);
}

TEST(FaceTweakOp, RefusesAGarbageBlob) {
    FaceTweakOp op;
    EXPECT_FALSE(op.deserializeParams(""));
    EXPECT_FALSE(op.deserializeParams("body=0;brep=0:"));            // no transform
    EXPECT_FALSE(op.deserializeParams("body=0;xf=nan,0,0,0,0,1,0,0,0,0,1,0;brep=0:"));
    EXPECT_FALSE(op.deserializeParams("body=0;an=0,0,0;xf=1,0,0,0,0,1,0,0,0,0,1,0;brep=0:"));
}
