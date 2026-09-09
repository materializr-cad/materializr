// #53: an extrude remembers WHICH regions it used. The normal workflow -
// draw, extrude, draw MORE in the same sketch, extrude again - must replay
// each extrude from its own regions, not the sketch's final everything-
// compound (which made four different extrudes replay identically).
#include <gtest/gtest.h>
#include "core/Document.h"
#include "core/History.h"
#include "modeling/ExtrudeOp.h"
#include "modeling/Sketch.h"
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <memory>

using namespace materializr;

static void addRect(Sketch& sk, float x0, float y0, float x1, float y1) {
    int a = sk.addPoint({x0, y0}), b = sk.addPoint({x1, y0});
    int c = sk.addPoint({x1, y1}), d = sk.addPoint({x0, y1});
    sk.addLine(a, b); sk.addLine(b, c); sk.addLine(c, d); sk.addLine(d, a);
}
static double vol(const TopoDS_Shape& s) {
    GProp_GProps g; BRepGProp::VolumeProperties(s, g); return g.Mass();
}

TEST(ExtrudeRegions, DrawExtrudeDrawMoreReplaysEachFromItsOwnRegions) {
    Document doc;
    History hist;
    auto sk = std::make_shared<Sketch>();
    sk->setPlane(gp_Pln(gp_Ax3(gp_Pnt(0,0,0), gp_Dir(0,1,0), gp_Dir(1,0,0))));
    addRect(*sk, 0, 0, 10, 10);                    // region A (100 mm²)
    int sid = doc.addSketch(sk, "Sketch 1");

    auto e1 = std::make_unique<ExtrudeOp>();
    e1->setSketchSource(sid);
    ASSERT_TRUE(e1->rebuildProfileFromSketch(doc));
    e1->setDistance(5.0);
    e1->setMode(ExtrudeMode::NewBody);
    ASSERT_TRUE(e1->execute(doc));
    hist.pushExecuted(std::move(e1));

    // MORE geometry in the same sketch AFTER the first extrude.
    addRect(*sk, 20, 0, 26, 4);                    // region B (24 mm²)
    auto e2 = std::make_unique<ExtrudeOp>();
    e2->setSketchSource(sid);
    ASSERT_TRUE(e2->rebuildProfileFromSketch(doc));
    // rebuild takes ALL regions at creation time; the app hands the op the
    // PICKED region - simulate by re-deriving then keeping only B via the
    // recorded points mechanism: point capture happens at execute from the
    // profile, so give it B's face only.
    {
        // Build B alone through a scratch sketch to get its face.
        Sketch skB;
        skB.setPlane(sk->getPlane());
        addRect(skB, 20, 0, 26, 4);
        e2->setProfile(skB.buildProfileShape());
    }
    e2->setDistance(3.0);
    e2->setMode(ExtrudeMode::NewBody);
    ASSERT_TRUE(e2->execute(doc));
    hist.pushExecuted(std::move(e2));

    auto ids = doc.getAllBodyIds();
    ASSERT_EQ(ids.size(), 2u);
    const double vA = vol(doc.getBody(ids[0]));    // 100*5 = 500
    const double vB = vol(doc.getBody(ids[1]));    // 24*3 = 72
    EXPECT_NEAR(vA, 500.0, 1e-6);
    EXPECT_NEAR(vB, 72.0, 1e-6);

    // Full replay: each extrude must rebuild from ITS regions.
    ASSERT_TRUE(hist.editStep(0, doc, /*transactional=*/true));
    EXPECT_NEAR(vol(doc.getBody(ids[0])), 500.0, 1e-6)
        << "extrude 1 grabbed later-drawn regions on replay";
    EXPECT_NEAR(vol(doc.getBody(ids[1])), 72.0, 1e-6);
}
