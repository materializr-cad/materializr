// 1.8.0-beta.5 tester: resizing the circle of an extrude-cut that was drawn ON a
// body face left a pancake - a disc of the old hole's size - standing in the
// middle of the enlarged hole, and circles added afterwards were ignored.
//
// A sketch on a body face carries the face's own boundary. Once the extrude had
// cut its hole, the face had that hole, and the drawn circle made bigger was cut
// into a ring around the OLD hole: the "contours drawn since" path then cut the
// ring and the face's leftover, not the whole new disc.
#include <gtest/gtest.h>
#include "core/Document.h"
#include "core/History.h"
#include "modeling/ExtrudeOp.h"
#include "modeling/Sketch.h"
#include <BRepBndLib.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <Bnd_Box.hxx>
#include <GProp_GProps.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <cmath>
#include <memory>

using namespace materializr;

static double vol(const TopoDS_Shape& s) { GProp_GProps g; BRepGProp::VolumeProperties(s, g); return g.Mass(); }
static int solids(const TopoDS_Shape& s) { int n = 0; for (TopExp_Explorer e(s, TopAbs_SOLID); e.More(); e.Next()) ++n; return n; }

TEST(ExtrudeHostFace, EnlargedHoleHasNoPancakeAndFollowsNewCircles) {
    Document doc;
    History hist;
    const double plateR = 25.0, plateH = 2.0;
    const int body = doc.addBody(BRepPrimAPI_MakeCylinder(plateR, plateH).Shape(), "Plate");
    const double plateVol = vol(doc.getBody(body));

    auto sk = std::make_shared<Sketch>();
    sk->setPlane(gp_Pln(gp_Ax3(gp_Pnt(0, 0, plateH), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0))));
    const int centre = sk->addPoint({0, 0});
    const int circle = sk->addCircle(centre, 4.0);
    const int sid = doc.addSketch(sk, "Sketch");

    auto ex = std::make_unique<ExtrudeOp>();
    ex->setSketchSource(sid);
    ASSERT_TRUE(ex->rebuildProfileFromSketch(doc));
    ex->setDistance(-plateH);
    ex->setMode(ExtrudeMode::Subtract);
    ex->setTargetBody(body);
    ASSERT_TRUE(ex->execute(doc));
    ExtrudeOp* raw = ex.get();
    hist.pushExecuted(std::move(ex));
    EXPECT_NEAR(vol(doc.getBody(body)), plateVol - M_PI * 16.0 * plateH, 1e-3);

    // Bind the sketch to the plate's top face AS IT IS NOW - with the hole in it.
    for (TopExp_Explorer fx(doc.getBody(body), TopAbs_FACE); fx.More(); fx.Next()) {
        Bnd_Box bb; BRepBndLib::Add(fx.Current(), bb);
        double a, b, c, d, e, f; bb.Get(a, b, c, d, e, f);
        if (std::abs(c - plateH) < 1e-6 && std::abs(f - plateH) < 1e-6) sk->setSourceFace(TopoDS::Face(fx.Current()));
    }
    ASSERT_FALSE(sk->getSourceFace().IsNull());

    // Make the hole bigger, as the sketch edit does, and let the cascade rebuild.
    sk->setCircleRadius(circle, 14.0);
    ASSERT_TRUE(raw->rebuildProfileFromSketch(doc, /*adoptNewRegions=*/true));
    ASSERT_TRUE(hist.editStep(0, doc, /*transactional=*/true));
    TopoDS_Shape b1 = doc.getBody(body);
    EXPECT_NEAR(vol(b1), plateVol - M_PI * 196.0 * plateH, 1e-2) << "a disc of the old hole was left behind";
    EXPECT_EQ(solids(b1), 1) << "the old hole's disc stands as a separate solid (the pancake)";

    // And a circle drawn afterwards is cut too.
    sk->addCircle(sk->addPoint({20, 0}), 1.5);
    ASSERT_TRUE(raw->rebuildProfileFromSketch(doc, true));
    ASSERT_TRUE(hist.editStep(0, doc, true));
    TopoDS_Shape b2 = doc.getBody(body);
    EXPECT_NEAR(vol(b2), plateVol - M_PI * 196.0 * plateH - M_PI * 2.25 * plateH, 1e-2);
    EXPECT_EQ(solids(b2), 1);
}
