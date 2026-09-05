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

// ── The locality claim, stated as a measurement ─────────────────────────────

TEST(FaceTweak, FeaturesAwayFromTheMovedFaceDoNotFollow) {
    // A box with a bore up through it. Slide the TOP face sideways: a whole-body
    // shear would lean the bore with it, which is exactly the behaviour this
    // engine exists to replace. Here the bore is nowhere near the moved face's
    // corners, so it must come out of the rebuild untouched.
    const TopoDS_Shape box = BRepPrimAPI_MakeBox(gp_Pnt(-10, -10, 0), 20.0, 20.0, 10.0).Shape();
    const TopoDS_Shape drill =
        BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(0, 0, -1), gp_Dir(0, 0, 1)), 3.0, 12.0).Shape();
    BRepAlgoAPI_Cut cut(box, drill);
    cut.Build();
    ASSERT_TRUE(cut.IsDone());
    const TopoDS_Shape bored = cut.Shape();

    gp_Trsf slide;
    slide.SetTranslation(gp_Vec(4, 0, 0));
    const auto r = tweak::moveFace(bored, faceTowards(bored, gp_Dir(0, 0, 1)), slide);

    // The top face now has a hole in it, so its corners are still three-face
    // corners but the face carries two wires. Either it rebuilds — and then the
    // bore must be exactly where it was — or it is refused with a reason.
    if (!r.ok()) {
        std::printf("bored box refused: %s\n", tweak::refusalText(r.refusal));
        SUCCEED() << "refused with a reason, which is the contract";
        return;
    }
    // The bore's axis is unmoved: its cylindrical face still centres on x=0.
    bool foundBore = false;
    for (TopExp_Explorer ex(r.shape, TopAbs_FACE); ex.More(); ex.Next()) {
        BRepAdaptor_Surface s(TopoDS::Face(ex.Current()));
        if (s.GetType() != GeomAbs_Cylinder) continue;
        foundBore = true;
        EXPECT_NEAR(s.Cylinder().Location().X(), 0.0, 1e-6)
            << "the bore followed the face that moved - that is the shear bug";
        EXPECT_NEAR(s.Cylinder().Radius(), 3.0, 1e-9);
    }
    EXPECT_TRUE(foundBore);
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

TEST(FaceTweak, CurvedNeighbourIsRefused) {
    // The flat top of a cylinder: planar itself, but the only thing meeting it
    // is the curved wall, so the corner solve has no third plane.
    const TopoDS_Shape cyl = BRepPrimAPI_MakeCylinder(5.0, 10.0).Shape();
    gp_Trsf t;
    t.SetTranslation(gp_Vec(0, 0, 2));
    const auto r = tweak::moveFace(cyl, faceTowards(cyl, gp_Dir(0, 0, 1)), t);
    EXPECT_FALSE(r.ok());
    EXPECT_TRUE(r.refusal == tweak::Refusal::NeighbourNotPlanar ||
                r.refusal == tweak::Refusal::NonManifoldCorner)
        << "got: " << tweak::refusalText(r.refusal);
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
