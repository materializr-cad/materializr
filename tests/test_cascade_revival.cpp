// A sketch cascade that can't make a step follow a sketch edit switches the
// step off. That used to be permanent: drag a lathe profile across its axis,
// the Lathe (and the cut and union built on it) went dark, and stayed dark
// after the profile was dragged back. Cascade-suspended steps now get another
// try the next time their sketch changes.

#include "core/Document.h"
#include "core/History.h"
#include "core/SketchCascadeTarget.h"
#include "modeling/ExtrudeOp.h"
#include "modeling/Sketch.h"

#include <gtest/gtest.h>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <gp_Ax3.hxx>
#include <gp_Pln.hxx>
#include <memory>

using namespace materializr;

namespace {

int addRectSketch(Document& doc, double x0, double y0, double w, double h) {
    auto sk = std::make_shared<Sketch>();
    sk->setPlane(gp_Pln(gp_Ax3(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0))));
    int p0 = sk->addPoint({(float)x0, (float)y0});
    int p1 = sk->addPoint({(float)(x0 + w), (float)y0});
    int p2 = sk->addPoint({(float)(x0 + w), (float)(y0 + h)});
    int p3 = sk->addPoint({(float)x0, (float)(y0 + h)});
    sk->addLine(p0, p1); sk->addLine(p1, p2); sk->addLine(p2, p3); sk->addLine(p3, p0);
    return doc.addSketch(sk);
}

bool pushExtrude(History& hist, Document& doc, int sketchId, double dist) {
    auto ext = std::make_unique<ExtrudeOp>();
    ext->setSketchSource(sketchId);
    ext->setDistance(dist);
    if (!ext->rebuildProfileFromSketch(doc)) return false;
    return hist.pushOperation(std::move(ext), doc);
}

// What Application::cascadeFromSketchEdit does when a step can't follow.
void suspend(History& hist, Document& doc, int step) {
    ASSERT_TRUE(hist.setStepEnabled(step, false, doc));
    const_cast<Operation*>(hist.getStep(step))->setCascadeSuspended(true);
}

double volumeOf(const TopoDS_Shape& s) {
    GProp_GProps g;
    BRepGProp::VolumeProperties(s, g);
    return g.Mass();
}

} // namespace

TEST(CascadeRevival, SuspendedStepComesBackWhenItsSketchIsEditedAgain) {
    Document doc;
    History hist;
    const int sidA = addRectSketch(doc, 0, 0, 10, 10);
    const int sidB = addRectSketch(doc, 100, 0, 10, 10);
    ASSERT_TRUE(pushExtrude(hist, doc, sidA, 5));   // step 0
    ASSERT_TRUE(pushExtrude(hist, doc, sidB, 5));   // step 1
    ASSERT_EQ(doc.getAllBodyIds().size(), 2u);

    suspend(hist, doc, 0);
    suspend(hist, doc, 1);
    ASSERT_TRUE(doc.getAllBodyIds().empty());

    const std::vector<int> reviving = reviveSuspendedFor(hist, sidA);
    ASSERT_EQ(reviving.size(), 2u) << "the step after it rides along";
    EXPECT_EQ(reviving.front(), 0);

    int matched = 0;
    int earliest = rederiveSketchDrivenOps(hist, doc, sidA, &matched);
    ASSERT_EQ(earliest, 0);
    ASSERT_TRUE(hist.editStep(earliest, doc, /*transactional=*/true));
    EXPECT_EQ(settleRevived(hist, reviving, true), 2);

    EXPECT_EQ(doc.getAllBodyIds().size(), 2u);
    for (int id : doc.getAllBodyIds()) EXPECT_NEAR(volumeOf(doc.getBody(id)), 500.0, 1e-3);
    for (int i = 0; i < 2; ++i) {
        EXPECT_TRUE(hist.getStep(i)->isEnabled());
        EXPECT_FALSE(hist.getStep(i)->isCascadeSuspended());
        EXPECT_FALSE(hist.getStep(i)->isReviving());
    }
}

TEST(CascadeRevival, OnlyStepsFromThisSketchOnwardAreRevived) {
    Document doc;
    History hist;
    const int sidA = addRectSketch(doc, 0, 0, 10, 10);
    const int sidB = addRectSketch(doc, 100, 0, 10, 10);
    ASSERT_TRUE(pushExtrude(hist, doc, sidA, 5));   // step 0
    ASSERT_TRUE(pushExtrude(hist, doc, sidB, 5));   // step 1
    suspend(hist, doc, 0);
    suspend(hist, doc, 1);

    const std::vector<int> reviving = reviveSuspendedFor(hist, sidB);
    ASSERT_EQ(reviving.size(), 1u);
    EXPECT_EQ(reviving.front(), 1);
    EXPECT_FALSE(hist.getStep(0)->isEnabled()) << "step 0 belongs to another sketch";
    settleRevived(hist, reviving, false);
}

TEST(CascadeRevival, UserDisabledStepsAreNeverRevived) {
    Document doc;
    History hist;
    const int sidA = addRectSketch(doc, 0, 0, 10, 10);
    ASSERT_TRUE(pushExtrude(hist, doc, sidA, 5));
    ASSERT_TRUE(hist.setStepEnabled(0, false, doc));    // the user's own toggle

    EXPECT_TRUE(reviveSuspendedFor(hist, sidA).empty());
    EXPECT_FALSE(hist.getStep(0)->isEnabled());
}

TEST(CascadeRevival, FailedRetryPutsTheStepBackToSuspended) {
    Document doc;
    History hist;
    const int sidA = addRectSketch(doc, 0, 0, 10, 10);
    ASSERT_TRUE(pushExtrude(hist, doc, sidA, 5));
    suspend(hist, doc, 0);

    const std::vector<int> reviving = reviveSuspendedFor(hist, sidA);
    ASSERT_EQ(reviving.size(), 1u);
    EXPECT_TRUE(hist.getStep(0)->isEnabled());
    EXPECT_TRUE(hist.getStep(0)->isReviving());

    EXPECT_EQ(settleRevived(hist, reviving, false), 0);
    EXPECT_FALSE(hist.getStep(0)->isEnabled());
    EXPECT_TRUE(hist.getStep(0)->isCascadeSuspended()) << "still eligible for the next try";
    EXPECT_FALSE(hist.getStep(0)->isReviving());
    EXPECT_TRUE(doc.getAllBodyIds().empty()) << "nothing was applied";
}

TEST(CascadeRevival, RollbackDoesNotUndoAStepThatWasNeverApplied) {
    // Step 0 is applied, step 1 is suspended-and-reviving. Rolling the chain
    // back from step 0 must not run undo() on step 1: it isn't in the document.
    Document doc;
    History hist;
    const int sidA = addRectSketch(doc, 0, 0, 10, 10);
    const int sidB = addRectSketch(doc, 100, 0, 10, 10);
    ASSERT_TRUE(pushExtrude(hist, doc, sidA, 5));
    ASSERT_TRUE(pushExtrude(hist, doc, sidB, 5));
    suspend(hist, doc, 1);
    ASSERT_EQ(doc.getAllBodyIds().size(), 1u);

    const std::vector<int> reviving = reviveSuspendedFor(hist, sidB);
    ASSERT_EQ(reviving.size(), 1u);
    ASSERT_EQ(rederiveSketchDrivenOps(hist, doc, sidB), 1);
    ASSERT_TRUE(hist.editStep(0, doc, /*transactional=*/true));
    EXPECT_EQ(settleRevived(hist, reviving, true), 1);
    EXPECT_EQ(doc.getAllBodyIds().size(), 2u);
}

// --- Ctrl+Z inside a reopened sketch ---------------------------------------
// Reopening a sketch drew in earlier used to put the undo floor at the step it
// was opened on, so the lines from the last visit could not be undone from
// inside the sketch (they could from the 3D view). The floor now drops below
// the run of the sketch's own edits that ends at the current step.

#include "modeling/SketchEditOp.h"

namespace {

int drawCircleStep(Document& doc, History& hist, std::shared_ptr<Sketch> live, int sid, double x) {
    auto before = std::make_shared<Sketch>(*live);
    live->addCircle(live->addPoint({(float)x, 0}), 2.0);
    auto after = std::make_shared<Sketch>(*live);
    auto op = std::make_unique<SketchEditOp>(live, before, after);
    op->setSketchId(sid);
    hist.pushExecuted(std::move(op), doc);
    return hist.currentStep();
}

} // namespace

TEST(SketchUndoFloor, ReachesBackThroughThisSketchsOwnEdits) {
    Document doc;
    History hist;
    const int bodySketch = addRectSketch(doc, 0, 0, 10, 10);
    ASSERT_TRUE(pushExtrude(hist, doc, bodySketch, 5));          // step 0: a body

    auto live = std::make_shared<Sketch>();
    const int sid = doc.addSketch(live, "S");
    drawCircleStep(doc, hist, live, sid, 0);                     // step 1
    drawCircleStep(doc, hist, live, sid, 10);                    // step 2

    // Reopened: undo may take steps 2 and 1, but never the extrude under them.
    EXPECT_EQ(sketchUndoFloor(hist, doc, sid), 0);
}

TEST(SketchUndoFloor, StopsAtAnyStepThatIsNotThisSketchs) {
    Document doc;
    History hist;
    auto live = std::make_shared<Sketch>();
    const int sid = doc.addSketch(live, "S");
    auto other = std::make_shared<Sketch>();
    const int osid = doc.addSketch(other, "Other");

    drawCircleStep(doc, hist, live, sid, 0);                     // step 0
    drawCircleStep(doc, hist, other, osid, 0);                   // step 1: another sketch
    drawCircleStep(doc, hist, live, sid, 10);                    // step 2

    EXPECT_EQ(sketchUndoFloor(hist, doc, sid), 1) << "the other sketch's edit is a wall";
}

TEST(SketchUndoFloor, AFreshSketchKeepsTheEntryStepAsItsFloor) {
    Document doc;
    History hist;
    const int bodySketch = addRectSketch(doc, 0, 0, 10, 10);
    ASSERT_TRUE(pushExtrude(hist, doc, bodySketch, 5));
    auto live = std::make_shared<Sketch>();
    const int sid = doc.addSketch(live, "New");
    EXPECT_EQ(sketchUndoFloor(hist, doc, sid), hist.currentStep());
}
