// Steve 2026-08-04: a cylinder extruded from a circle sketch, pushed back
// DOWN (shorter) with Push/Pull, doesn't shorten cleanly - a sliver of the
// old outer wall survives around ~half the rim. Scale Face on the same body
// mangles the side wall. Move Face and Tilt are fine. Reproduce both at the
// modeling level; the UI preview is a real op execution, so if it's real it
// should show here.
#include <gtest/gtest.h>
#include "core/Document.h"
#include "modeling/Sketch.h"
#include "modeling/ExtrudeOp.h"
#include "modeling/PushPullOp.h"
#include "modeling/ScaleFaceOp.h"
#include <BRepCheck_Analyzer.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <gp_Pln.hxx>
#include <gp_Ax3.hxx>
#include <cstdio>
#include <memory>
using namespace materializr;

namespace {

constexpr double kPi = 3.14159265358979;

double vol(const TopoDS_Shape& s) {
    GProp_GProps g; BRepGProp::VolumeProperties(s, g); return g.Mass();
}
int faceCount(const TopoDS_Shape& s) {
    int n = 0; for (TopExp_Explorer x(s, TopAbs_FACE); x.More(); x.Next()) ++n;
    return n;
}
void dumpFaces(const TopoDS_Shape& s) {
    for (TopExp_Explorer x(s, TopAbs_FACE); x.More(); x.Next()) {
        const TopoDS_Face& f = TopoDS::Face(x.Current());
        BRepAdaptor_Surface surf(f);
        GProp_GProps g; BRepGProp::SurfaceProperties(f, g);
        std::printf("  face type=%d area=%.3f centre=(%.2f,%.2f,%.2f)\n",
                    (int)surf.GetType(), g.Mass(),
                    g.CentreOfMass().X(), g.CentreOfMass().Y(),
                    g.CentreOfMass().Z());
    }
}
TopoDS_Face planarFaceAtZ(const TopoDS_Shape& s, double z) {
    for (TopExp_Explorer x(s, TopAbs_FACE); x.More(); x.Next()) {
        const TopoDS_Face& f = TopoDS::Face(x.Current());
        BRepAdaptor_Surface surf(f);
        if (surf.GetType() != GeomAbs_Plane) continue;
        GProp_GProps g; BRepGProp::SurfaceProperties(f, g);
        if (std::abs(g.CentreOfMass().Z() - z) < 1e-6) return f;
    }
    return {};
}

// Circle r=6 on the XY plane, extruded +Z by 40 → the cylinder Steve made.
struct CircleCyl {
    Document doc;
    int bodyId = -1;
    TopoDS_Face top;
    double vol0 = 0.0;
    CircleCyl() {
        auto sk = std::make_shared<Sketch>();
        sk->setPlane(gp_Pln(gp_Ax3(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1),
                                   gp_Dir(1, 0, 0))));
        int c = sk->addPoint(glm::vec2(0, 0));
        sk->addCircle(c, 6.0f);
        int sid = doc.addSketch(sk, "Sketch 1");

        ExtrudeOp op;
        op.setSketchSource(sid);
        op.setDistance(40.0);
        op.setMode(ExtrudeMode::NewBody);
        if (!op.rebuildProfileFromSketch(doc)) return;
        if (!op.execute(doc)) return;
        for (int id : doc.getAllBodyIds()) bodyId = id;
        if (bodyId < 0) return;
        TopoDS_Shape body = doc.getBody(bodyId);
        top = planarFaceAtZ(body, 40.0);
        vol0 = vol(body);
        std::printf("base cylinder: vol=%.3f (ideal %.3f) faces=%d\n",
                    vol0, kPi * 36.0 * 40.0, faceCount(body));
        dumpFaces(body);
    }
};

} // namespace

TEST(CirclePushPullScale, PushDownShortensCleanly) {
    CircleCyl f;
    ASSERT_GE(f.bodyId, 0);
    ASSERT_FALSE(f.top.IsNull());

    PushPullOp op;
    op.setTargets({{f.top, f.bodyId}});
    op.setDistance(-31.0);
    ASSERT_TRUE(op.execute(f.doc));

    TopoDS_Shape r = f.doc.getBody(f.bodyId);
    const double v = vol(r);
    const double expect = kPi * 36.0 * 9.0; // shortened to h=9
    std::printf("after push -31: vol=%.3f (expect %.3f) faces=%d\n",
                v, expect, faceCount(r));
    dumpFaces(r);
    EXPECT_TRUE(BRepCheck_Analyzer(r).IsValid());
    EXPECT_NEAR(v, expect, expect * 0.01);
    // A clean shortened cylinder is cap + cap + wall. A surviving wall
    // sliver shows up as extra faces.
    EXPECT_EQ(faceCount(r), 3);
}

TEST(CirclePushPullScale, ScaleShrinkKeepsWallSane) {
    CircleCyl f;
    ASSERT_GE(f.bodyId, 0);
    ASSERT_FALSE(f.top.IsNull());

    ScaleFaceOp op;
    op.setBody(f.bodyId);
    op.setFace(f.top);
    op.setMode(ScaleFaceOp::Mode::Pinch);
    op.setScalePercent(70.0);
    op.setLength(40.0); // full depth: walls re-slope from the base
    ASSERT_TRUE(op.execute(f.doc));

    TopoDS_Shape r = f.doc.getBody(f.bodyId);
    const double R = 6.0, rr = 4.2;
    const double expect = kPi * 40.0 / 3.0 * (R * R + R * rr + rr * rr);
    const double v = vol(r);
    std::printf("after pinch 70%%: vol=%.3f (expect %.3f) faces=%d\n",
                v, expect, faceCount(r));
    dumpFaces(r);
    EXPECT_TRUE(BRepCheck_Analyzer(r).IsValid());
    // Uniform scale of a circle must stay EXACT - an analytic cone wall,
    // volume to boolean-fuzz precision, cap still on axis.
    EXPECT_NEAR(v, expect, expect * 0.001);
    EXPECT_EQ(faceCount(r), 3);
    TopoDS_Face newTop = planarFaceAtZ(r, 40.0);
    ASSERT_FALSE(newTop.IsNull());
    GProp_GProps tg; BRepGProp::SurfaceProperties(newTop, tg);
    EXPECT_NEAR(tg.CentreOfMass().X(), 0.0, 1e-3);
    EXPECT_NEAR(tg.CentreOfMass().Y(), 0.0, 1e-3);
}

TEST(CirclePushPullScale, ScaleGrowKeepsWallSane) {
    CircleCyl f;
    ASSERT_GE(f.bodyId, 0);
    ASSERT_FALSE(f.top.IsNull());

    ScaleFaceOp op;
    op.setBody(f.bodyId);
    op.setFace(f.top);
    op.setMode(ScaleFaceOp::Mode::Pinch);
    op.setScalePercent(130.0);
    op.setLength(40.0);
    ASSERT_TRUE(op.execute(f.doc));

    TopoDS_Shape r = f.doc.getBody(f.bodyId);
    const double R = 6.0, rr = 7.8;
    const double expect = kPi * 40.0 / 3.0 * (R * R + R * rr + rr * rr);
    const double v = vol(r);
    std::printf("after grow 130%%: vol=%.3f (expect %.3f) faces=%d\n",
                v, expect, faceCount(r));
    dumpFaces(r);
    EXPECT_TRUE(BRepCheck_Analyzer(r).IsValid());
    EXPECT_NEAR(v, expect, expect * 0.001);
    // ONE cap on top - the Fuse used to leave the original disc AND a
    // coplanar annulus stacked at z=40 (the gray ring in Steve's shot).
    EXPECT_EQ(faceCount(r), 3);
    TopoDS_Face newTop = planarFaceAtZ(r, 40.0);
    ASSERT_FALSE(newTop.IsNull());
    GProp_GProps tg; BRepGProp::SurfaceProperties(newTop, tg);
    EXPECT_NEAR(tg.Mass(), kPi * rr * rr, kPi * rr * rr * 0.001);
    EXPECT_NEAR(tg.CentreOfMass().X(), 0.0, 1e-3);
    EXPECT_NEAR(tg.CentreOfMass().Y(), 0.0, 1e-3);
}

// Steve's combined flow: scale first, then push the (now smaller) top face
// back down. On a frustum the cut prism only spans the top radius, so a
// conical shoulder remains all the way round - that part is geometry, not a
// bug - but the result must be valid and symmetric, never the half-moon
// sliver the off-axis bspline cap produced.
TEST(CirclePushPullScale, ScaleThenPushStaysSymmetric) {
    CircleCyl f;
    ASSERT_GE(f.bodyId, 0);
    ASSERT_FALSE(f.top.IsNull());

    ScaleFaceOp sc;
    sc.setBody(f.bodyId);
    sc.setFace(f.top);
    sc.setMode(ScaleFaceOp::Mode::Pinch);
    sc.setScalePercent(70.0);
    sc.setLength(40.0);
    ASSERT_TRUE(sc.execute(f.doc));

    TopoDS_Face newTop = planarFaceAtZ(f.doc.getBody(f.bodyId), 40.0);
    ASSERT_FALSE(newTop.IsNull());

    PushPullOp op;
    op.setTargets({{newTop, f.bodyId}});
    op.setDistance(-31.0);
    ASSERT_TRUE(op.execute(f.doc));

    TopoDS_Shape r = f.doc.getBody(f.bodyId);
    std::printf("after pinch+push: vol=%.3f faces=%d\n", vol(r), faceCount(r));
    dumpFaces(r);
    EXPECT_TRUE(BRepCheck_Analyzer(r).IsValid());
    // Symmetry: the whole result must stay centred on the axis.
    GProp_GProps g; BRepGProp::VolumeProperties(r, g);
    EXPECT_NEAR(g.CentreOfMass().X(), 0.0, 1e-3);
    EXPECT_NEAR(g.CentreOfMass().Y(), 0.0, 1e-3);
}
