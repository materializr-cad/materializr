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
