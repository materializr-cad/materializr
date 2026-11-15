// 11-3-26 (1.8.0.b2_2_2, _5): a contour drawn in a sketch AFTER a feature was
// made from it was ignored by Push/Pull and Extrude - the feature kept only the
// regions it started with (#53), so the new circle never reached the body, not
// even after an Undo/Redo. A feature is a feature of the SKETCH: a closed shape
// drawn later gets the same distance. The guard rails:
//   - a region that was already there and left out stays out;
//   - nothing adopts when more than one feature is built from the sketch;
//   - the cascade is the only thing that asks (a plain rebuild is unchanged).
#include <gtest/gtest.h>
#include "core/Document.h"
#include "core/History.h"
#include "core/SketchCascadeTarget.h"
#include "modeling/ExtrudeOp.h"
#include "modeling/PushPullOp.h"
#include "modeling/RevolveOp.h"
#include "modeling/Sketch.h"
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <cmath>
#include <memory>

using namespace materializr;

namespace {
double vol(const TopoDS_Shape& s) {
    GProp_GProps g; BRepGProp::VolumeProperties(s, g); return std::abs(g.Mass());
}
double totalVolume(const Document& doc) {
    double v = 0.0;
    for (int id : doc.getAllBodyIds()) v += vol(doc.getBody(id));
    return v;
}
gp_Pln groundPlane() { return gp_Pln(gp_Ax3(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0))); }
void addCircleAt(Sketch& sk, glm::vec2 c, double r) { sk.addCircle(sk.addPoint(c), r); }
const double kPi = 3.14159265358979323846;
double disc(double r) { return kPi * r * r; }

// Push every drawn region of the sketch, the way the whole-sketch Push does.
std::unique_ptr<PushPullOp> pushAll(Document& doc, int sid, double dist) {
    auto sk = doc.getSketch(sid);
    auto regions = sk->buildRegions();
    const auto drawn = sk->drawnRegions(regions);
    auto op = std::make_unique<PushPullOp>();
    std::vector<PushPullOp::Target> ts(drawn.size());
    op->setTargets(ts);
    op->setDistance(dist);
    for (size_t i = 0; i < drawn.size(); ++i)
        op->setSketchSource(static_cast<int>(i), sid, drawn[i], regions[drawn[i]].representativePoint);
    return op;
}
// What the cascade does: re-derive, then re-run.
double replay(PushPullOp& op, Document& doc, int sid, bool adopt) {
    op.undo(doc);
    EXPECT_TRUE(op.rebuildProfileFromSketch(doc, sid, adopt));
    EXPECT_TRUE(op.execute(doc));
    return totalVolume(doc);
}
} // namespace

TEST(RegionAdoption, PushPullTakesOnASeparateCircleDrawnLater) {
    Document doc;
    auto sk = std::make_shared<Sketch>();
    sk->setPlane(groundPlane());
    addCircleAt(*sk, {0, 0}, 5);
    addCircleAt(*sk, {20, 0}, 5);
    const int sid = doc.addSketch(sk);
    auto op = pushAll(doc, sid, 4.0);
    ASSERT_TRUE(op->rebuildProfileFromSketch(doc, sid));
    ASSERT_TRUE(op->execute(doc));
    EXPECT_NEAR(totalVolume(doc), 2 * disc(5) * 4.0, 1.0);

    addCircleAt(*sk, {40, 0}, 5);            // the third circle
    EXPECT_NEAR(replay(*op, doc, sid, /*adopt=*/false), 2 * disc(5) * 4.0, 1.0)
        << "a plain rebuild keeps its regions (unchanged behaviour)";
    EXPECT_NEAR(replay(*op, doc, sid, /*adopt=*/true), 3 * disc(5) * 4.0, 1.0)
        << "the cascade gives the new circle the same push";
    EXPECT_NEAR(replay(*op, doc, sid, true), 3 * disc(5) * 4.0, 1.0) << "and it stays one target";
}

TEST(RegionAdoption, PushPullTakesOnAConcentricCircleDrawnLater) {
    // 1.8.0.b2_2_2: a circle added around the pushed one made a new ANNULUS.
    Document doc;
    auto sk = std::make_shared<Sketch>();
    sk->setPlane(groundPlane());
    addCircleAt(*sk, {0, 0}, 5);
    const int sid = doc.addSketch(sk);
    auto op = pushAll(doc, sid, 3.0);
    ASSERT_TRUE(op->rebuildProfileFromSketch(doc, sid));
    ASSERT_TRUE(op->execute(doc));
    EXPECT_NEAR(totalVolume(doc), disc(5) * 3.0, 1.0);

    addCircleAt(*sk, {0, 0}, 10);
    EXPECT_NEAR(replay(*op, doc, sid, true), disc(10) * 3.0, 2.0);
}

TEST(RegionAdoption, ARegionLeftOutAtTheStartStaysOut) {
    Document doc;
    auto sk = std::make_shared<Sketch>();
    sk->setPlane(groundPlane());
    addCircleAt(*sk, {0, 0}, 5);             // pushed
    addCircleAt(*sk, {20, 0}, 5);            // there at the start, deliberately not pushed
    const int sid = doc.addSketch(sk);
    auto regions = sk->buildRegions();
    int first = -1;
    for (size_t i = 0; i < regions.size(); ++i)
        if (glm::length(regions[i].representativePoint) < 6.0f) first = static_cast<int>(i);
    ASSERT_GE(first, 0);
    PushPullOp op;
    op.setTargets({PushPullOp::Target{}});
    op.setDistance(4.0);
    op.setSketchSource(0, sid, first, regions[first].representativePoint);
    ASSERT_TRUE(op.rebuildProfileFromSketch(doc, sid));
    ASSERT_TRUE(op.execute(doc));
    EXPECT_NEAR(totalVolume(doc), disc(5) * 4.0, 1.0);

    addCircleAt(*sk, {40, 0}, 5);
    EXPECT_NEAR(replay(op, doc, sid, true), 2 * disc(5) * 4.0, 1.0)
        << "the new circle joins; the one left out at the start does not";
}

TEST(RegionAdoption, KnownRegionsSurviveASaveAndReload) {
    Document doc;
    auto sk = std::make_shared<Sketch>();
    sk->setPlane(groundPlane());
    addCircleAt(*sk, {0, 0}, 5);
    const int sid = doc.addSketch(sk);
    auto op = pushAll(doc, sid, 4.0);
    ASSERT_TRUE(op->rebuildProfileFromSketch(doc, sid));
    ASSERT_TRUE(op->execute(doc));
    const std::string blob = op->serializeParams();
    EXPECT_NE(blob.find(";kp" + std::to_string(sid) + "="), std::string::npos) << blob;

    PushPullOp again;
    ASSERT_TRUE(again.deserializeParams(blob));
    addCircleAt(*sk, {20, 0}, 5);
    ASSERT_TRUE(again.rebuildProfileFromSketch(doc, sid, true));
    EXPECT_EQ(again.targetCount(), 2) << "the reloaded op still knows what was there";
}

TEST(RegionAdoption, AFileFromBeforeTheMemoryExistedNeverAdopts) {
    Document doc;
    auto sk = std::make_shared<Sketch>();
    sk->setPlane(groundPlane());
    addCircleAt(*sk, {0, 0}, 5);
    const int sid = doc.addSketch(sk);
    auto op = pushAll(doc, sid, 4.0);
    ASSERT_TRUE(op->rebuildProfileFromSketch(doc, sid));
    ASSERT_TRUE(op->execute(doc));
    std::string blob = op->serializeParams();
    blob.erase(blob.find(";kp"));            // an older save

    PushPullOp old;
    ASSERT_TRUE(old.deserializeParams(blob));
    addCircleAt(*sk, {20, 0}, 5);
    ASSERT_TRUE(old.rebuildProfileFromSketch(doc, sid, true));
    EXPECT_EQ(old.targetCount(), 1);
}

TEST(RegionAdoption, ExtrudeTakesOnAContourDrawnLaterButNotOneLeftOut) {
    Document doc;
    auto sk = std::make_shared<Sketch>();
    sk->setPlane(groundPlane());
    addCircleAt(*sk, {0, 0}, 5);             // extruded
    addCircleAt(*sk, {20, 0}, 5);            // there at the start, left out
    const int sid = doc.addSketch(sk);
    Sketch only;
    only.setPlane(groundPlane());
    addCircleAt(only, {0, 0}, 5);

    ExtrudeOp ex;
    ex.setSketchSource(sid);
    ex.setProfile(only.buildProfileShape());
    ex.setDistance(6.0);
    ex.setMode(ExtrudeMode::NewBody);
    ASSERT_TRUE(ex.execute(doc));
    EXPECT_NEAR(totalVolume(doc), disc(5) * 6.0, 1.0);

    addCircleAt(*sk, {40, 0}, 5);
    ex.undo(doc);
    ASSERT_TRUE(ex.rebuildProfileFromSketch(doc, /*adopt=*/true));
    ASSERT_TRUE(ex.execute(doc));
    EXPECT_NEAR(totalVolume(doc), 2 * disc(5) * 6.0, 2.0);
}

TEST(RegionAdoption, ExtrudeTakesOnAConcentricCircleDrawnLater) {
    Document doc;
    auto sk = std::make_shared<Sketch>();
    sk->setPlane(groundPlane());
    addCircleAt(*sk, {0, 0}, 5);
    const int sid = doc.addSketch(sk);
    ExtrudeOp ex;
    ex.setSketchSource(sid);
    ASSERT_TRUE(ex.rebuildProfileFromSketch(doc));
    ex.setDistance(6.0);
    ex.setMode(ExtrudeMode::NewBody);
    ASSERT_TRUE(ex.execute(doc));

    addCircleAt(*sk, {0, 0}, 10);
    ex.undo(doc);
    ASSERT_TRUE(ex.rebuildProfileFromSketch(doc, true));
    ASSERT_TRUE(ex.execute(doc));
    EXPECT_NEAR(totalVolume(doc), disc(10) * 6.0, 3.0);
}

// With two features built from one sketch the MOST RECENT takes a new contour;
// the earlier one leaves it alone, or it would be built twice.
TEST(RegionAdoption, OnlyTheLatestOfTwoFeaturesTakesANewContour) {
    Document doc;
    History hist;
    auto sk = std::make_shared<Sketch>();
    sk->setPlane(groundPlane());
    addCircleAt(*sk, {0, 0}, 5);
    addCircleAt(*sk, {20, 0}, 5);
    const int sid = doc.addSketch(sk);
    auto regions = sk->buildRegions();

    auto op1 = std::make_unique<PushPullOp>();
    op1->setTargets({PushPullOp::Target{}});
    op1->setDistance(4.0);
    op1->setSketchSource(0, sid, 0, regions[0].representativePoint);
    ASSERT_TRUE(op1->rebuildProfileFromSketch(doc, sid));
    PushPullOp* p1 = op1.get();
    ASSERT_TRUE(hist.pushOperation(std::move(op1), doc));

    auto op2 = std::make_unique<PushPullOp>();
    op2->setTargets({PushPullOp::Target{}});
    op2->setDistance(2.0);
    op2->setSketchSource(0, sid, 1, regions[1].representativePoint);
    ASSERT_TRUE(op2->rebuildProfileFromSketch(doc, sid));
    PushPullOp* p2 = op2.get();
    ASSERT_TRUE(hist.pushOperation(std::move(op2), doc));

    addCircleAt(*sk, {40, 0}, 5);
    rederiveSketchDrivenOps(hist, doc, sid);
    EXPECT_EQ(p1->targetCount(), 1) << "the earlier feature leaves the new contour alone";
    EXPECT_EQ(p2->targetCount(), 2) << "the latest feature takes it";
}

TEST(RegionAdoption, TheCascadeAdoptsWhenTheSketchHasOneFeature) {
    Document doc;
    History hist;
    auto sk = std::make_shared<Sketch>();
    sk->setPlane(groundPlane());
    addCircleAt(*sk, {0, 0}, 5);
    const int sid = doc.addSketch(sk);
    auto op = pushAll(doc, sid, 4.0);
    ASSERT_TRUE(op->rebuildProfileFromSketch(doc, sid));
    PushPullOp* raw = op.get();
    ASSERT_TRUE(hist.pushOperation(std::move(op), doc));

    addCircleAt(*sk, {20, 0}, 5);
    EXPECT_GE(rederiveSketchDrivenOps(hist, doc, sid), 0);
    EXPECT_EQ(raw->targetCount(), 2);
}

// 1.8.0 beta tester list 11-14-26 (item 4): a dome / cylinder made by a Lathe, a
// Subtract extrude of two circles through its top, then seven more circles drawn
// in that sketch (a circular pattern). The body must pick up all the new holes
// through the cascade, without a save and reload.
TEST(RegionAdoption, SubtractExtrudeThroughALatheTakesPatternCirclesDrawnLater) {
    Document doc; History hist;
    // Lathe profile: a 30 x 25 rectangle on XY, revolved about Y -> a cylinder r=30, h=25.
    auto prof = std::make_shared<Sketch>();
    prof->setPlane(groundPlane());
    const int a = prof->addPoint({0, 0}), b = prof->addPoint({30, 0}),
              c = prof->addPoint({30, 25}), d = prof->addPoint({0, 25});
    prof->addLine(a, b); prof->addLine(b, c); prof->addLine(c, d); prof->addLine(d, a);
    const int psid = doc.addSketch(prof, "Profile");
    {
        auto regions = prof->buildRegions();
        auto op = std::make_unique<RevolveOp>();
        op->setProfile(regions.front().face);
        op->setSketchSource(psid);
        op->setAxis(gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(0, 1, 0)));
        op->setAngle(360.0);
        op->setMode(RevolveMode::NewBody);
        ASSERT_TRUE(hist.pushOperation(std::move(op), doc));
    }
    const int bid = doc.getAllBodyIds().front();
    const double base = vol(doc.getBody(bid));
    ASSERT_NEAR(base, kPi * 30 * 30 * 25, 5.0);

    // The hole sketch: on the top face plane (y = 25, normal +Y).
    auto hs = std::make_shared<Sketch>();
    hs->setPlane(gp_Pln(gp_Ax3(gp_Pnt(0, 25, 0), gp_Dir(0, 1, 0), gp_Dir(1, 0, 0))));
    const int centreP = hs->addPoint({0, 0});
    hs->addCircle(centreP, 4.0);                         // the big centre hole
    const int smallP = hs->addPoint({15, 0});
    hs->addCircle(smallP, 1.5);                          // one small hole
    hs->addLine(centreP, smallP);                        // the tester's line between them
    const int hsid = doc.addSketch(hs, "Holes");

    auto cut = std::make_unique<ExtrudeOp>();
    cut->setSketchSource(hsid);
    ASSERT_TRUE(cut->rebuildProfileFromSketch(doc));
    cut->setDistance(-25.0);
    cut->setMode(ExtrudeMode::Subtract);
    cut->setTargetBody(bid);
    ASSERT_TRUE(hist.pushOperation(std::move(cut), doc));
    const double twoHoles = vol(doc.getBody(bid));
    EXPECT_NEAR(twoHoles, base - (kPi * 16 + kPi * 2.25) * 25.0, 20.0) << "setup: two holes cut";

    // Seven more small circles round the centre, as a circular pattern would.
    for (int i = 1; i < 8; ++i) {
        const double ang = i * kPi / 4.0;
        addCircleAt(*hs, {static_cast<float>(15 * std::cos(ang)), static_cast<float>(15 * std::sin(ang))}, 1.5);
    }
    // What the app's cascade does.
    int matched = 0;
    const int earliest = rederiveSketchDrivenOps(hist, doc, hsid, &matched);
    ASSERT_GE(earliest, 0) << "the cut must be re-derived (matched " << matched << ")";
    doc.setCascadeSketchOverride(hsid, std::make_shared<Sketch>(*hs));
    ASSERT_TRUE(hist.editStep(earliest, doc, /*transactional=*/true));
    doc.clearCascadeSketchOverrides();
    EXPECT_NEAR(vol(doc.getBody(bid)), base - (kPi * 16 + 8 * kPi * 2.25) * 25.0, 20.0)
        << "all eight small holes + the centre hole";
}

// The fingerprint the app compares at sketch Finish to know whether the bodies were
// rebuilt from the sketch's CURRENT state (Application::exitSketchMode). It has to
// see every kind of edit - in particular what a circular pattern adds (new circles
// and points) and a Duplicate / rotate (moved points) - and ignore a pure copy.
#include "core/SketchSignature.h"
TEST(SketchSignature, SeesEveryEditAndIgnoresACopy) {
    Sketch sk;
    sk.setPlane(groundPlane());
    const int c = sk.addPoint({0, 0});
    const int circ = sk.addCircle(c, 3.0);
    const size_t base = sketchGeometrySignature(sk);
    EXPECT_EQ(sketchGeometrySignature(Sketch(sk)), base) << "a copy is the same sketch";

    { Sketch t(sk); t.addCircle(t.addPoint({10, 0}), 1.5);
      EXPECT_NE(sketchGeometrySignature(t), base) << "a circle added by a pattern"; }
    { Sketch t(sk); t.movePoint(c, {0.5f, 0});
      EXPECT_NE(sketchGeometrySignature(t), base) << "a moved point (gizmo rotate / move)"; }
    { Sketch t(sk); t.addLine(t.addPoint({5, 5}), t.addPoint({9, 5}));
      EXPECT_NE(sketchGeometrySignature(t), base) << "a line added by a duplicate"; }
    (void)circ;
}
