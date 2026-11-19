// Issue #185: moving a sketch moves the body built from it, but a sketch drawn
// ON that body (and everything built from that second sketch) used to be left
// behind - its features then failed to follow and were switched off, and Undo
// did not bring them back. The move now carries hosted sketches along in the
// same SketchTransformOp, and the cascade re-reads their profiles in the same
// replay.

#include "core/Document.h"
#include "core/History.h"
#include "core/SketchCascadeTarget.h"
#include "modeling/ExtrudeOp.h"
#include "modeling/PushPullOp.h"
#include "modeling/Sketch.h"
#include "modeling/SketchTransformOp.h"

#include <gtest/gtest.h>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <Bnd_Box.hxx>
#include <BRepBndLib.hxx>
#include <gp_Ax3.hxx>
#include <gp_Pln.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>
#include <memory>

using namespace materializr;

namespace {

int addRect(Document& doc, double z, double x0, double y0, double w, double h, int sourceBody) {
    auto sk = std::make_shared<Sketch>();
    sk->setPlane(gp_Pln(gp_Ax3(gp_Pnt(0, 0, z), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0))));
    int p0 = sk->addPoint({(float)x0, (float)y0});
    int p1 = sk->addPoint({(float)(x0 + w), (float)y0});
    int p2 = sk->addPoint({(float)(x0 + w), (float)(y0 + h)});
    int p3 = sk->addPoint({(float)x0, (float)(y0 + h)});
    sk->addLine(p0, p1); sk->addLine(p1, p2); sk->addLine(p2, p3); sk->addLine(p3, p0);
    if (sourceBody >= 0) sk->setSourceBody(sourceBody);
    return doc.addSketch(sk);
}

bool pushExtrude(History& hist, Document& doc, int sketchId, double dist) {
    auto ext = std::make_unique<ExtrudeOp>();
    ext->setSketchSource(sketchId);
    ext->setDistance(dist);
    if (!ext->rebuildProfileFromSketch(doc)) return false;
    return hist.pushOperation(std::move(ext), doc);
}

double minZ(const TopoDS_Shape& s) {
    Bnd_Box b; BRepBndLib::Add(s, b);
    double x0, y0, z0, x1, y1, z1; b.Get(x0, y0, z0, x1, y1, z1);
    return z0;
}

// The shape of Application::cascadeFromSketchEdit: rederive every sketch in
// the group, replay once from the earliest rebuilt step.
bool cascade(History& hist, Document& doc, int sid) {
    auto links = computeSketchBodyLinks(hist);
    std::vector<int> group{sid};
    for (int f : hostedFollowerSketches(links, hist, doc, sid)) group.push_back(f);
    int earliest = -1;
    for (int g : group) {
        int e = rederiveSketchDrivenOps(hist, doc, g);
        if (e >= 0 && (earliest < 0 || e < earliest)) earliest = e;
    }
    return earliest < 0 || hist.editStep(earliest, doc, /*transactional=*/true);
}

struct Scene {
    Document doc;
    History hist;
    int s1 = -1, s2 = -1, b1 = -1, b2 = -1;
    Scene() {
        s1 = addRect(doc, 0, -10, -10, 20, 20, -1);
        EXPECT_TRUE(pushExtrude(hist, doc, s1, 25));        // body 1: z 0..25
        b1 = doc.getAllBodyIds().front();
        s2 = addRect(doc, 25, -5, -5, 10, 10, b1);          // drawn on B1's top face
        EXPECT_TRUE(pushExtrude(hist, doc, s2, 10));        // body 2 stacked on top
        for (int id : doc.getAllBodyIds()) if (id != b1) b2 = id;
    }
};

std::unique_ptr<SketchTransformOp> moveOp(int sid, double dz) {
    auto op = std::make_unique<SketchTransformOp>();
    op->setSketch(sid);
    gp_Trsf t; t.SetTranslation(gp_Vec(0, 0, dz));
    op->setTransform(t);
    return op;
}

} // namespace

TEST(SketchMoveHosted, FindsTheSketchDrawnOnTheBodyItBuilds) {
    Scene sc;
    auto links = computeSketchBodyLinks(sc.hist);
    EXPECT_EQ(hostedFollowerSketches(links, sc.hist, sc.doc, sc.s1), std::vector<int>{sc.s2});
    EXPECT_TRUE(hostedFollowerSketches(links, sc.hist, sc.doc, sc.s2).empty())
        << "nothing is drawn on the stacked body";
    EXPECT_TRUE(hostedFollowerSketches(links, sc.hist, sc.doc, sc.s1, {sc.s2}).empty())
        << "a sketch already being moved is not carried twice";
}

TEST(SketchMoveHosted, HostedSketchAndItsFeaturesFollowTheMove) {
    Scene sc;
    ASSERT_NE(sc.b2, -1);
    ASSERT_NEAR(minZ(sc.doc.getBody(sc.b2)), 25.0, 1e-6);

    auto op = moveOp(sc.s1, 40);
    addHostedFollowers(*op, computeSketchBodyLinks(sc.hist), sc.hist, sc.doc, sc.s1);
    ASSERT_EQ(op->followerCount(), 1);
    ASSERT_TRUE(sc.hist.pushOperation(std::move(op), sc.doc));
    ASSERT_TRUE(cascade(sc.hist, sc.doc, sc.s1));

    EXPECT_NEAR(sc.doc.getSketch(sc.s2)->getPlane().Location().Z(), 65.0, 1e-9);
    EXPECT_NEAR(minZ(sc.doc.getBody(sc.b1)), 40.0, 1e-6);
    EXPECT_NEAR(minZ(sc.doc.getBody(sc.b2)), 65.0, 1e-6)
        << "the feature built from the hosted sketch moved with it";
    for (int i = 0; i < sc.hist.stepCount(); ++i)
        EXPECT_TRUE(sc.hist.getStep(i)->isEnabled()) << "step " << i << " was switched off";
}

TEST(SketchMoveHosted, UndoPutsEverythingBack) {
    Scene sc;
    auto op = moveOp(sc.s1, 40);
    addHostedFollowers(*op, computeSketchBodyLinks(sc.hist), sc.hist, sc.doc, sc.s1);
    ASSERT_TRUE(sc.hist.pushOperation(std::move(op), sc.doc));
    ASSERT_TRUE(cascade(sc.hist, sc.doc, sc.s1));

    ASSERT_TRUE(sc.hist.undo(sc.doc));
    ASSERT_TRUE(cascade(sc.hist, sc.doc, sc.s1));    // what the undo dispatch does

    EXPECT_NEAR(sc.doc.getSketch(sc.s1)->getPlane().Location().Z(), 0.0, 1e-9);
    EXPECT_NEAR(sc.doc.getSketch(sc.s2)->getPlane().Location().Z(), 25.0, 1e-9);
    EXPECT_NEAR(minZ(sc.doc.getBody(sc.b1)), 0.0, 1e-6);
    EXPECT_NEAR(minZ(sc.doc.getBody(sc.b2)), 25.0, 1e-6);

    ASSERT_TRUE(sc.hist.redo(sc.doc));
    ASSERT_TRUE(cascade(sc.hist, sc.doc, sc.s1));
    EXPECT_NEAR(minZ(sc.doc.getBody(sc.b2)), 65.0, 1e-6);
}

TEST(SketchMoveHosted, DetachedSketchCarriesNothing) {
    Scene sc;
    sc.doc.getSketch(sc.s1)->setDetachedFromBody(true);
    auto op = moveOp(sc.s1, 40);
    addHostedFollowers(*op, computeSketchBodyLinks(sc.hist), sc.hist, sc.doc, sc.s1);
    EXPECT_EQ(op->followerCount(), 0);
}

TEST(SketchMoveHosted, FollowersSurviveSaveAndReload) {
    Scene sc;
    auto op = moveOp(sc.s1, 40);
    addHostedFollowers(*op, computeSketchBodyLinks(sc.hist), sc.hist, sc.doc, sc.s1);
    ASSERT_TRUE(op->execute(sc.doc));
    const std::string blob = op->serializeParams();

    SketchTransformOp back;
    ASSERT_TRUE(back.deserializeParams(blob));
    ASSERT_EQ(back.followerCount(), 1);
    EXPECT_EQ(back.getFollowerId(0), sc.s2);

    // Replaying the reloaded op from the pre-move scene lands both sketches
    // where the original did, and undo restores the captured poses.
    Scene fresh;
    ASSERT_TRUE(back.execute(fresh.doc));
    EXPECT_NEAR(fresh.doc.getSketch(fresh.s1)->getPlane().Location().Z(), 40.0, 1e-9);
    EXPECT_NEAR(fresh.doc.getSketch(fresh.s2)->getPlane().Location().Z(), 65.0, 1e-9);
    ASSERT_TRUE(back.undo(fresh.doc));
    EXPECT_NEAR(fresh.doc.getSketch(fresh.s2)->getPlane().Location().Z(), 25.0, 1e-9);
}

// The reporter's actual case: Sketch 2 holds a pocket cut INTO the body Sketch 1
// built. Before the fix the move left Sketch 2 behind, the pocket step could not
// follow, and it was switched off - the body came back without its pocket.
TEST(SketchMoveHosted, PocketCutFromAHostedSketchFollowsTheBody) {
    Document doc;
    History hist;
    const int s1 = addRect(doc, 0, -10, -10, 20, 20, -1);
    ASSERT_TRUE(pushExtrude(hist, doc, s1, 25));                   // body: z 0..25
    const int b1 = doc.getAllBodyIds().front();
    const int s2 = addRect(doc, 25, -2, -2, 4, 4, b1);             // 4x4 pocket on top

    auto sk2 = doc.getSketch(s2);
    auto regions = sk2->buildRegions();
    const auto drawn = sk2->drawnRegions(regions);
    ASSERT_EQ(drawn.size(), 1u);
    auto pp = std::make_unique<PushPullOp>();
    std::vector<PushPullOp::Target> ts(1);
    ts[0].sourceBodyId = b1;
    pp->setTargets(ts);
    pp->setDistance(-10.0);
    pp->setCutIntersecting(true);
    pp->setSketchSource(0, s2, drawn[0], regions[drawn[0]].representativePoint);
    ASSERT_TRUE(pp->rebuildProfileFromSketch(doc, s2));
    ASSERT_TRUE(hist.pushOperation(std::move(pp), doc));

    auto vol = [&](int id) { GProp_GProps g; BRepGProp::VolumeProperties(doc.getBody(id), g); return g.Mass(); };
    const double pocketed = 20.0 * 20.0 * 25.0 - 4.0 * 4.0 * 10.0;
    ASSERT_NEAR(vol(b1), pocketed, 1e-3);

    auto op = moveOp(s1, 40);
    addHostedFollowers(*op, computeSketchBodyLinks(hist), hist, doc, s1);
    ASSERT_EQ(op->followerCount(), 1);
    ASSERT_TRUE(hist.pushOperation(std::move(op), doc));
    ASSERT_TRUE(cascade(hist, doc, s1));

    EXPECT_NEAR(minZ(doc.getBody(b1)), 40.0, 1e-6);
    EXPECT_NEAR(vol(b1), pocketed, 1e-3) << "the pocket must still be there after the move";
    for (int i = 0; i < hist.stepCount(); ++i)
        EXPECT_TRUE(hist.getStep(i)->isEnabled()) << "step " << i << " was switched off";

    ASSERT_TRUE(hist.undo(doc));
    ASSERT_TRUE(cascade(hist, doc, s1));
    EXPECT_NEAR(minZ(doc.getBody(b1)), 0.0, 1e-6);
    EXPECT_NEAR(vol(b1), pocketed, 1e-3) << "undo must bring the pocket back at the old place";
}

// Control for the test above: with the hosted sketch NOT carried, the pocket
// step cannot follow the moved body, the replay fails at that step and the
// cascade reports it. That is the "couldn't update the model" error #185
// described - kept so the test above is known to be sensitive to the fix.
TEST(SketchMoveHosted, WithoutFollowersThePocketStepCannotFollow) {
    Document doc;
    History hist;
    const int s1 = addRect(doc, 0, -10, -10, 20, 20, -1);
    ASSERT_TRUE(pushExtrude(hist, doc, s1, 25));
    const int b1 = doc.getAllBodyIds().front();
    const int s2 = addRect(doc, 25, -2, -2, 4, 4, b1);
    auto sk2 = doc.getSketch(s2);
    auto regions = sk2->buildRegions();
    const auto drawn = sk2->drawnRegions(regions);
    auto pp = std::make_unique<PushPullOp>();
    std::vector<PushPullOp::Target> ts(1);
    ts[0].sourceBodyId = b1;
    pp->setTargets(ts);
    pp->setDistance(-10.0);
    pp->setCutIntersecting(true);
    pp->setSketchSource(0, s2, drawn[0], regions[drawn[0]].representativePoint);
    ASSERT_TRUE(pp->rebuildProfileFromSketch(doc, s2));
    ASSERT_TRUE(hist.pushOperation(std::move(pp), doc));

    ASSERT_TRUE(hist.pushOperation(moveOp(s1, 40), doc));      // no followers
    EXPECT_FALSE(cascade(hist, doc, s1))
        << "the pocket step cannot follow a body its sketch was left behind by";
}

// The app heals a "severed" body link by re-adopting a body that has a face
// coplanar with the sketch - which a base sketch of a moved body can satisfy
// (its plane meets the body's base face). That made Sketch 1 look hosted on the
// body it built, and moving Sketch 2 would have dragged Sketch 1 (and so the
// body) along. A follower must come LATER in the history than what it follows.
TEST(SketchMoveHosted, BaseSketchReAdoptedByItsBodyIsNotDraggedByTheSketchOnTop) {
    Scene sc;
    sc.doc.getSketch(sc.s1)->setSourceBody(sc.b1);          // the heal's result
    auto links = computeSketchBodyLinks(sc.hist);
    EXPECT_TRUE(hostedFollowerSketches(links, sc.hist, sc.doc, sc.s2).empty())
        << "moving the sketch on top must not drag the base sketch";
    EXPECT_EQ(hostedFollowerSketches(links, sc.hist, sc.doc, sc.s1), std::vector<int>{sc.s2})
        << "the normal direction still works";
}

TEST(SketchMoveHosted, SketchOnTheBodyWithNoFeatureYetStillTravels) {
    Scene sc;
    // A third sketch drawn on the body but not extruded yet.
    const int s3 = addRect(sc.doc, 25, 12, 12, 4, 4, sc.b1);
    auto links = computeSketchBodyLinks(sc.hist);
    const auto f = hostedFollowerSketches(links, sc.hist, sc.doc, sc.s1);
    EXPECT_EQ(f, (std::vector<int>{sc.s2, s3}));
}
