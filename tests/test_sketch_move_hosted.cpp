// Issue #185: moving a sketch moves the body built from it, but a sketch drawn
// ON that body (and everything built from that second sketch) used to be left
// behind - its features then failed to follow and were switched off, and Undo
// did not bring them back. The move now carries hosted sketches along in the
// same SketchTransformOp, and the cascade re-reads their profiles in the same
// replay.

#include "core/Document.h"
#include "core/History.h"
#include "core/SketchCascadeTarget.h"
#include "io/ProjectIO.h"
#include "modeling/BatchTransformOp.h"
#include "modeling/ExtrudeOp.h"
#include "modeling/PushPullOp.h"
#include "modeling/Sketch.h"
#include "modeling/SketchEditOp.h"
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


// ---------------------------------------------------------------------------
// Issue #185, second half: the reporter did not use the Move gizmo - he opened
// Sketch 1, selected every line, dragged them and finished. That is a
// SketchEditOp, not a SketchTransformOp: the body rebuilt in the new place and
// every sketch drawn on it stayed behind. A drag of the WHOLE sketch now carries
// them, recorded on the edit step.
// ---------------------------------------------------------------------------

namespace {

std::shared_ptr<Sketch> snapshot(const Document& doc, int sid) {
    return std::make_shared<Sketch>(*doc.getSketch(sid));
}

void shiftAllPoints(Sketch& sk, glm::vec2 d) {
    std::vector<std::pair<int, glm::vec2>> moves;
    for (const auto& p : sk.getPoints()) moves.push_back({p.id, p.pos + d});
    for (const auto& [id, pos] : moves) sk.movePoint(id, pos);
}

double minX(const TopoDS_Shape& s) {
    Bnd_Box b; BRepBndLib::Add(s, b);
    double x0, y0, z0, x1, y1, z1; b.Get(x0, y0, z0, x1, y1, z1);
    return x0;
}

// What Application::carryHostedSketchesWithEdit + cascadeFromSketchEdit do for
// a freshly recorded edit of `sid`.
bool recordEditAndCascade(Scene& sc, int sid, std::shared_ptr<Sketch> before) {
    auto after = snapshot(sc.doc, sid);
    auto op = std::make_unique<SketchEditOp>(sc.doc.getSketch(sid), before, after);
    SketchEditOp* raw = op.get();
    sc.hist.pushExecuted(std::move(op), sc.doc);
    gp_Trsf world;
    if (sketchRigidMotion(*before, *after, world))
        for (int f : hostedFollowerSketches(computeSketchBodyLinks(sc.hist), sc.hist, sc.doc, sid)) {
            auto fs = sc.doc.getSketch(f);
            gp_Pln moved = fs->getPlane();
            moved.Transform(world);
            raw->addFollowerMove(f, fs->getPlane(), moved);
            fs->setPlane(moved);
        }
    return cascade(sc.hist, sc.doc, sid);
}

} // namespace

TEST(SketchEditCarriesHosted, RigidMotionDetectorAcceptsAShiftAndATurn) {
    Scene sc;
    auto before = snapshot(sc.doc, sc.s1);
    gp_Trsf t;

    auto shifted = std::make_shared<Sketch>(*before);
    shiftAllPoints(*shifted, {30.f, -5.f});
    ASSERT_TRUE(sketchRigidMotion(*before, *shifted, t));
    EXPECT_NEAR(t.TranslationPart().X(), 30.0, 1e-9);
    EXPECT_NEAR(t.TranslationPart().Y(), -5.0, 1e-9);

    // A quarter turn about the sketch's own origin.
    auto turned = std::make_shared<Sketch>(*before);
    for (const auto& p : before->getPoints()) turned->movePoint(p.id, {-p.pos.y, p.pos.x});
    ASSERT_TRUE(sketchRigidMotion(*before, *turned, t));
    gp_Pnt probe(10, 0, 0);
    probe.Transform(t);
    EXPECT_NEAR(probe.X(), 0.0, 1e-6);
    EXPECT_NEAR(probe.Y(), 10.0, 1e-6);
}

TEST(SketchEditCarriesHosted, RigidMotionDetectorRejectsEverythingElse) {
    Scene sc;
    auto before = snapshot(sc.doc, sc.s1);
    gp_Trsf t;

    EXPECT_FALSE(sketchRigidMotion(*before, *before, t)) << "nothing moved";

    auto onePoint = std::make_shared<Sketch>(*before);
    onePoint->movePoint(before->getPoints()[0].id, before->getPoints()[0].pos + glm::vec2(5.f, 0.f));
    EXPECT_FALSE(sketchRigidMotion(*before, *onePoint, t)) << "one dragged corner is a reshape";

    auto scaled = std::make_shared<Sketch>(*before);
    for (const auto& p : before->getPoints()) scaled->movePoint(p.id, p.pos * 1.1f);
    EXPECT_FALSE(sketchRigidMotion(*before, *scaled, t)) << "a resize is not a move";

    auto extra = std::make_shared<Sketch>(*before);
    extra->addPoint({1.f, 1.f});
    EXPECT_FALSE(sketchRigidMotion(*before, *extra, t)) << "a new point changes the set";

    auto replaned = std::make_shared<Sketch>(*before);
    shiftAllPoints(*replaned, {3.f, 0.f});
    replaned->setPlane(gp_Pln(gp_Ax3(gp_Pnt(0, 0, 9), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0))));
    EXPECT_FALSE(sketchRigidMotion(*before, *replaned, t)) << "plane changed too";
}

TEST(SketchEditCarriesHosted, DraggingTheWholeSketchTakesTheSketchOnItsBodyAlong) {
    Scene sc;
    ASSERT_NEAR(minX(sc.doc.getBody(sc.b2)), -5.0, 1e-6);
    auto before = snapshot(sc.doc, sc.s1);
    shiftAllPoints(*sc.doc.getSketch(sc.s1), {30.f, 0.f});       // the drag
    ASSERT_TRUE(recordEditAndCascade(sc, sc.s1, before));

    EXPECT_NEAR(minX(sc.doc.getBody(sc.b1)), 20.0, 1e-6);
    EXPECT_NEAR(minX(sc.doc.getBody(sc.b2)), 25.0, 1e-6)
        << "the body built from the sketch on top moved with it";
    EXPECT_NEAR(sc.doc.getSketch(sc.s2)->getPlane().Location().X(), 30.0, 1e-9);
    for (int i = 0; i < sc.hist.stepCount(); ++i)
        EXPECT_TRUE(sc.hist.getStep(i)->isEnabled()) << "step " << i << " was switched off";
}

TEST(SketchEditCarriesHosted, UndoAndRedoMoveThemBackAndForth) {
    Scene sc;
    auto before = snapshot(sc.doc, sc.s1);
    shiftAllPoints(*sc.doc.getSketch(sc.s1), {30.f, 0.f});
    ASSERT_TRUE(recordEditAndCascade(sc, sc.s1, before));

    ASSERT_TRUE(sc.hist.undo(sc.doc));
    ASSERT_TRUE(cascade(sc.hist, sc.doc, sc.s1));
    EXPECT_NEAR(sc.doc.getSketch(sc.s2)->getPlane().Location().X(), 0.0, 1e-9);
    EXPECT_NEAR(minX(sc.doc.getBody(sc.b1)), -10.0, 1e-6);
    EXPECT_NEAR(minX(sc.doc.getBody(sc.b2)), -5.0, 1e-6);
    for (int i = 0; i < sc.hist.stepCount(); ++i)
        EXPECT_TRUE(sc.hist.getStep(i)->isEnabled()) << "step " << i;

    ASSERT_TRUE(sc.hist.redo(sc.doc));
    ASSERT_TRUE(cascade(sc.hist, sc.doc, sc.s1));
    EXPECT_NEAR(sc.doc.getSketch(sc.s2)->getPlane().Location().X(), 30.0, 1e-9);
    EXPECT_NEAR(minX(sc.doc.getBody(sc.b2)), 25.0, 1e-6);
}

TEST(SketchEditCarriesHosted, AReshapeLeavesTheHostedSketchAlone) {
    Scene sc;
    auto before = snapshot(sc.doc, sc.s1);
    auto* sk = sc.doc.getSketch(sc.s1).get();
    sk->movePoint(before->getPoints()[0].id, before->getPoints()[0].pos + glm::vec2(-2.f, -2.f));
    auto after = snapshot(sc.doc, sc.s1);
    gp_Trsf t;
    EXPECT_FALSE(sketchRigidMotion(*before, *after, t));
    EXPECT_NEAR(sc.doc.getSketch(sc.s2)->getPlane().Location().X(), 0.0, 1e-9);
}

TEST(SketchEditCarriesHosted, FollowersSurviveSaveAndReload) {
    Scene sc;
    auto before = snapshot(sc.doc, sc.s1);
    shiftAllPoints(*sc.doc.getSketch(sc.s1), {30.f, 0.f});
    ASSERT_TRUE(recordEditAndCascade(sc, sc.s1, before));

    auto* se = dynamic_cast<const SketchEditOp*>(sc.hist.getStep(sc.hist.currentStep()));
    ASSERT_NE(se, nullptr);
    ASSERT_EQ(se->followerMoves().size(), 1u);
    const std::string blob = se->serializeWithDocument(sc.doc);
    ASSERT_FALSE(blob.empty());

    Scene fresh;                                       // the pre-edit scene
    auto back = ProjectIO::rehydrateSketchEditOp(blob, fresh.doc);
    ASSERT_NE(back, nullptr);
    EXPECT_TRUE(back->followersAnalyzed());
    ASSERT_EQ(back->followerMoves().size(), 1u);
    EXPECT_EQ(back->followerMoves()[0].id, fresh.s2);
    ASSERT_TRUE(back->execute(fresh.doc));
    EXPECT_NEAR(fresh.doc.getSketch(fresh.s2)->getPlane().Location().X(), 30.0, 1e-9);
    ASSERT_TRUE(back->undo(fresh.doc));
    EXPECT_NEAR(fresh.doc.getSketch(fresh.s2)->getPlane().Location().X(), 0.0, 1e-9);
}


// ---------------------------------------------------------------------------
// Moving a BODY on its own: its sketch moves with it and the body re-derives;
// sketches drawn on it come along, except one that only made a separate body that
// is not being moved.
// ---------------------------------------------------------------------------

namespace {

std::unique_ptr<PushPullOp> pocketFrom(Document& doc, int s2, int b1, double dist) {
    auto sk2 = doc.getSketch(s2);
    auto regions = sk2->buildRegions();
    const auto drawn = sk2->drawnRegions(regions);
    auto pp = std::make_unique<PushPullOp>();
    std::vector<PushPullOp::Target> ts(1);
    ts[0].sourceBodyId = b1;
    pp->setTargets(ts);
    pp->setDistance(dist);
    pp->setCutIntersecting(true);
    pp->setSketchSource(0, s2, drawn[0], regions[drawn[0]].representativePoint);
    EXPECT_TRUE(pp->rebuildProfileFromSketch(doc, s2));
    return pp;
}

} // namespace

TEST(BodyMovePlan, TheSketchOnTopOnlyFollowsIfItTouchedAMovedBody) {
    Scene sc;                                           // s2 on b1 made a SEPARATE body b2
    auto links = computeSketchBodyLinks(sc.hist);

    auto onlyB1 = planBodyMove(links, sc.hist, sc.doc, {sc.b1});
    EXPECT_EQ(onlyB1.drivers, std::vector<int>{sc.s1});
    EXPECT_TRUE(onlyB1.followers.empty())
        << "it never touched b1 and made a separate body that is not moving - it stays";

    auto both = planBodyMove(links, sc.hist, sc.doc, {sc.b1, sc.b2});
    EXPECT_EQ(both.followers, std::vector<int>{sc.s2}) << "that body is moving too";
}

TEST(BodyMovePlan, ASketchThatCutTheBodyFollowsIt) {
    Document doc; History hist;
    const int s1 = addRect(doc, 0, -10, -10, 20, 20, -1);
    ASSERT_TRUE(pushExtrude(hist, doc, s1, 25));
    const int b1 = doc.getAllBodyIds().front();
    const int s2 = addRect(doc, 25, -2, -2, 4, 4, b1);
    ASSERT_TRUE(hist.pushOperation(pocketFrom(doc, s2, b1, -10.0), doc));
    const int s3 = addRect(doc, 25, 6, 6, 2, 2, b1);   // drawn on it, nothing built yet

    auto plan = planBodyMove(computeSketchBodyLinks(hist), hist, doc, {b1});
    EXPECT_EQ(plan.drivers, std::vector<int>{s1});
    EXPECT_EQ(plan.followers, (std::vector<int>{s2, s3}));
    EXPECT_TRUE(plan.blocked.empty());
}

TEST(BodyMovePlan, MovingTheBodyMovesItsSketchAndThePocketFollows) {
    Document doc; History hist;
    const int s1 = addRect(doc, 0, -10, -10, 20, 20, -1);
    ASSERT_TRUE(pushExtrude(hist, doc, s1, 25));
    const int b1 = doc.getAllBodyIds().front();
    const int s2 = addRect(doc, 25, -2, -2, 4, 4, b1);
    ASSERT_TRUE(hist.pushOperation(pocketFrom(doc, s2, b1, -10.0), doc));
    auto vol = [&](int id) { GProp_GProps g; BRepGProp::VolumeProperties(doc.getBody(id), g); return g.Mass(); };
    const double pocketed = 20.0 * 20.0 * 25.0 - 4.0 * 4.0 * 10.0;
    ASSERT_NEAR(vol(b1), pocketed, 1e-3);

    // What the gizmo commit does for a lone body move.
    auto plan = planBodyMove(computeSketchBodyLinks(hist), hist, doc, {b1});
    ASSERT_EQ(plan.drivers, std::vector<int>{s1});
    auto op = moveOp(s1, 40);
    for (int f : plan.followers) op->addFollower(f);
    ASSERT_TRUE(hist.pushOperation(std::move(op), doc));
    ASSERT_TRUE(cascade(hist, doc, s1));

    EXPECT_NEAR(minZ(doc.getBody(b1)), 40.0, 1e-6) << "the body moved";
    EXPECT_NEAR(vol(b1), pocketed, 1e-3) << "and still has its pocket";
    EXPECT_NEAR(doc.getSketch(s2)->getPlane().Location().Z(), 65.0, 1e-9);
    for (int i = 0; i < hist.stepCount(); ++i)
        EXPECT_TRUE(hist.getStep(i)->isEnabled()) << "step " << i << " was switched off";
}

TEST(BodyMovePlan, ASketchThatBuildsABodyThatStaysCannotBeMovedWithThisOne) {
    Scene sc;
    // The base sketch is extruded a second time into another body b3 that stays.
    ASSERT_TRUE(pushExtrude(sc.hist, sc.doc, sc.s1, 5));
    int b3 = -1;
    for (int id : sc.doc.getAllBodyIds()) if (id != sc.b1 && id != sc.b2) b3 = id;
    ASSERT_NE(b3, -1);
    auto plan = planBodyMove(computeSketchBodyLinks(sc.hist), sc.hist, sc.doc, {sc.b1});
    EXPECT_TRUE(plan.drivers.empty());
    EXPECT_EQ(plan.blocked, std::vector<int>{sc.s1})
        << "moving it would drag b3 along - so b1 is moved rigidly instead";
}

TEST(BodyMovePlan, DetachedSketchesAreLeftAlone) {
    Scene sc;
    sc.doc.getSketch(sc.s2)->setDetachedFromBody(true);
    auto plan = planBodyMove(computeSketchBodyLinks(sc.hist), sc.hist, sc.doc, {sc.b1, sc.b2});
    EXPECT_TRUE(plan.followers.empty());
    sc.doc.getSketch(sc.s1)->setDetachedFromBody(true);
    plan = planBodyMove(computeSketchBodyLinks(sc.hist), sc.hist, sc.doc, {sc.b1, sc.b2});
    EXPECT_TRUE(plan.drivers.empty());
}

TEST(BodyMovePlan, RigidBatchMoveCarriesTheSketchesDrawnOnTheBodies) {
    Scene sc;
    auto op = std::make_unique<BatchTransformOp>();
    op->setBodies({sc.b1});
    gp_Trsf t; t.SetTranslation(gp_Vec(0, 0, 40));
    op->setTransform(gp_GTrsf(t));
    op->addFollowSketch(sc.s2);
    ASSERT_TRUE(sc.hist.pushOperation(std::move(op), sc.doc));
    EXPECT_NEAR(minZ(sc.doc.getBody(sc.b1)), 40.0, 1e-6);
    EXPECT_NEAR(sc.doc.getSketch(sc.s2)->getPlane().Location().Z(), 65.0, 1e-9);
    ASSERT_TRUE(sc.hist.undo(sc.doc));
    EXPECT_NEAR(sc.doc.getSketch(sc.s2)->getPlane().Location().Z(), 25.0, 1e-9);
}
