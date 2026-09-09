// Cancelling a heavy operation from its progress reporter.
//
// The reporter is what Application::renderProgressFrame becomes when an
// operation runs between frames: it returns true once the user presses
// Cancel. These tests hold the two halves of the contract that makes that
// button safe. The kernel has to see the cancel (OpProgressBridge forwards
// it through UserBreak, and the algorithm aborts not-done), and the operation
// has to fail cleanly, leaving the document exactly as it found it - because
// History::pushOperation does NOT restore the document when execute() returns
// false, it only declines to record the step.
#include "core/Document.h"
#include "core/OpProgress.h"
#include "modeling/FaceLineage.h"
#include "modeling/PushPullOp.h"
#include "modeling/ShellOp.h"

#include <gtest/gtest.h>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRep_Builder.hxx>
#include <GProp_GProps.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <chrono>
#include <memory>
#include <utility>

namespace {

// A plate with a grid of holes. Enough faces that the boolean takes long
// enough to be cancelled part-way, few enough to keep the suite quick.
TopoDS_Shape plate(int nx, int ny) {
    TopoDS_Shape p = BRepPrimAPI_MakeBox(nx * 15.0, ny * 15.0, 10.0).Shape();
    TopoDS_Compound holes;
    BRep_Builder bb;
    bb.MakeCompound(holes);
    for (int i = 0; i < nx; ++i)
        for (int j = 0; j < ny; ++j)
            bb.Add(holes, BRepPrimAPI_MakeCylinder(
                              gp_Ax2(gp_Pnt(7.5 + i * 15.0, 7.5 + j * 15.0, -1.0),
                                     gp_Dir(0, 0, 1)),
                              3.0, 12.0)
                              .Shape());
    return BRepAlgoAPI_Cut(p, holes).Shape();
}

TopoDS_Face topFace(const TopoDS_Shape& s) {
    TopoDS_Face best;
    double bz = -1e9;
    for (TopExp_Explorer e(s, TopAbs_FACE); e.More(); e.Next()) {
        GProp_GProps g;
        BRepGProp::SurfaceProperties(e.Current(), g);
        if (g.CentreOfMass().Z() > bz && g.Mass() > 100) {
            bz = g.CentreOfMass().Z();
            best = TopoDS::Face(e.Current());
        }
    }
    return best;
}

double volumeOf(const TopoDS_Shape& s) {
    GProp_GProps g;
    BRepGProp::VolumeProperties(s, g);
    return g.Mass();
}

int faceCount(const TopoDS_Shape& s) {
    int n = 0;
    for (TopExp_Explorer e(s, TopAbs_FACE); e.More(); e.Next()) ++n;
    return n;
}

// Says "cancelled" from the Nth call onward, and records what it was asked.
//
// Which N matters more than it looks. Message_ProgressIndicator::Start() fires
// one Show() at fraction 0 BEFORE any algorithm runs, so a reporter that
// cancels on its first call latches before the target loop is even entered and
// no boolean is ever exercised - the operation still fails and the body is
// still untouched, so such a test passes while proving nothing about whether
// the kernel honours UserBreak. Cancelling from the SECOND call onward puts the
// latch inside a running algorithm, which is the behaviour worth pinning.
struct Reporter {
    int cancelAfter = 1;
    int calls = 0;
    float lastFraction = -99.0f;
    float maxFraction = -99.0f;
    bool operator()(float fraction, const char* label) {
        ++calls;
        lastFraction = fraction;
        if (fraction > maxFraction) maxFraction = fraction;
        EXPECT_NE(label, nullptr);
        return calls > cancelAfter;
    }
};

// Proof that a callback came from inside a running algorithm rather than from
// Start(): Start() always reports exactly 0.
#define ASSERT_KERNEL_DROVE_IT(rep)  EXPECT_KERNEL_DROVE_IT(rep)
#define EXPECT_KERNEL_DROVE_IT(rep)                                            \
    do {                                                                       \
        EXPECT_GT((rep)->calls, 1) << "only Start() reported; the cancel "     \
                                      "never reached a running algorithm";     \
        EXPECT_GT((rep)->maxFraction, 0.0f)                                    \
            << "every callback reported fraction 0, so none came from the "    \
               "kernel";                                                       \
    } while (0)

} // namespace

TEST(OpProgress, PushPullCancelLeavesTheBodyUntouched) {
    // Large enough that the boolean outlives several throttle windows, so the
    // cancel lands mid-algorithm rather than before it.
    const TopoDS_Shape base = plate(14, 12);
    Document doc;
    const int id = doc.addBody(base, "plate");
    const double vol0 = volumeOf(doc.getBody(id));
    const int faces0 = faceCount(doc.getBody(id));

    auto reporter = std::make_shared<Reporter>();
    reporter->cancelAfter = 1;   // let Start() through, cancel inside the cut

    PushPullOp op;
    PushPullOp::Target t;
    t.profile = topFace(base);
    t.sourceBodyId = id;
    op.setTargets({t});
    op.setDistance(5.0);
    op.setProgressReporter(
        [reporter](float f, const char* l) { return (*reporter)(f, l); });

    EXPECT_FALSE(op.execute(doc)) << "a cancelled operation must fail";
    EXPECT_KERNEL_DROVE_IT(reporter);
    EXPECT_DOUBLE_EQ(volumeOf(doc.getBody(id)), vol0);
    EXPECT_EQ(faceCount(doc.getBody(id)), faces0);
}

TEST(OpProgress, PushPullWithNoCancelStillSucceeds) {
    const TopoDS_Shape base = plate(8, 6);
    Document doc;
    const int id = doc.addBody(base, "plate");
    const double vol0 = volumeOf(doc.getBody(id));

    auto reporter = std::make_shared<Reporter>();
    reporter->cancelAfter = 1000000;   // never cancels

    PushPullOp op;
    PushPullOp::Target t;
    t.profile = topFace(base);
    t.sourceBodyId = id;
    op.setTargets({t});
    op.setDistance(5.0);
    op.setProgressReporter(
        [reporter](float f, const char* l) { return (*reporter)(f, l); });

    EXPECT_TRUE(op.execute(doc));
    EXPECT_GT(reporter->calls, 0) << "progress was never reported";
    EXPECT_GT(volumeOf(doc.getBody(id)), vol0) << "the push added no material";
}

// The case the rollback exists for. History::pushOperation does not restore
// the document when execute() returns false, so a gesture cancelled after an
// earlier target already landed would leave that body mutated with no history
// entry to undo it. The reporter here cancels the moment the first target's
// body changes, which is deterministic: it does not depend on how many
// callbacks the kernel happens to make.
TEST(OpProgress, PushPullCancelUndoesTargetsThatAlreadyLanded) {
    Document doc;
    const TopoDS_Shape a = plate(6, 5);
    gp_Trsf away;
    away.SetTranslation(gp_Vec(500, 0, 0));
    const TopoDS_Shape b = BRepBuilderAPI_Transform(plate(6, 5), away, true).Shape();
    const int idA = doc.addBody(a, "a");
    const int idB = doc.addBody(b, "b");
    const double volA = volumeOf(doc.getBody(idA));
    const double volB = volumeOf(doc.getBody(idB));

    PushPullOp op;
    PushPullOp::Target ta, tb;
    ta.profile = topFace(a);
    ta.sourceBodyId = idA;
    tb.profile = topFace(b);
    tb.sourceBodyId = idB;
    op.setTargets({ta, tb});
    op.setDistance(5.0);

    bool sawFirstLand = false;
    op.setProgressReporter([&](float, const char*) {
        if (volumeOf(doc.getBody(idA)) != volA) sawFirstLand = true;
        return sawFirstLand;
    });

    EXPECT_FALSE(op.execute(doc));
    ASSERT_TRUE(sawFirstLand) << "the first target never landed, so this test "
                                 "did not exercise the rollback";
    EXPECT_DOUBLE_EQ(volumeOf(doc.getBody(idA)), volA)
        << "the target that landed before the cancel was left applied";
    EXPECT_DOUBLE_EQ(volumeOf(doc.getBody(idB)), volB);
}

TEST(OpProgress, ShellCancelLeavesTheBodyUntouched) {
    const TopoDS_Shape base = plate(10, 8);
    Document doc;
    const int id = doc.addBody(base, "plate");
    const double vol0 = volumeOf(doc.getBody(id));
    const int faces0 = faceCount(doc.getBody(id));

    auto reporter = std::make_shared<Reporter>();
    reporter->cancelAfter = 1;   // see the Push/Pull case above

    ShellOp op;
    op.setBody(id);
    op.setThickness(1.0);
    op.addFaceToRemove(topFace(base));
    op.setProgressReporter(
        [reporter](float f, const char* l) { return (*reporter)(f, l); });

    EXPECT_FALSE(op.execute(doc)) << "a cancelled shell must fail";
    EXPECT_KERNEL_DROVE_IT(reporter);
    EXPECT_DOUBLE_EQ(volumeOf(doc.getBody(id)), vol0);
    EXPECT_EQ(faceCount(doc.getBody(id)), faces0);
}

// What UserBreak actually buys, and the only thing that observes it.
//
// The operations check the bridge's own latch too, so they fail cleanly and
// leave the body untouched even if the kernel ignores UserBreak entirely -
// every other test here passes either way. The difference UserBreak makes is
// WHEN: it aborts the algorithm mid-flight instead of letting it run to
// completion first. On a shell that is the difference between a Cancel that
// responds and one that appears dead for seconds, so it is worth pinning.
//
// Compared as a ratio between two runs in the same process, back to back,
// rather than against an absolute threshold, so a loaded machine moves both
// numbers together.
TEST(OpProgress, CancelAbortsTheAlgorithmInsteadOfWaitingForIt) {
    const TopoDS_Shape base = plate(10, 8);

    auto runShell = [&](bool cancel) {
        Document doc;
        const int id = doc.addBody(base, "plate");
        auto reporter = std::make_shared<Reporter>();
        reporter->cancelAfter = cancel ? 1 : 1000000;
        ShellOp op;
        op.setBody(id);
        op.setThickness(1.0);
        op.addFaceToRemove(topFace(base));
        op.setProgressReporter(
            [reporter](float f, const char* l) { return (*reporter)(f, l); });
        const auto t0 = std::chrono::steady_clock::now();
        op.execute(doc);
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t0).count();
        return std::make_pair(ms, reporter);
    };

    const auto full = runShell(false);
    const auto stopped = runShell(true);
    ASSERT_KERNEL_DROVE_IT(stopped.second);
    EXPECT_LT(stopped.first, full.first * 0.6)
        << "the cancelled shell took " << stopped.first << " ms against "
        << full.first << " ms uncancelled, so the algorithm ran to completion "
        << "and UserBreak is not reaching the kernel";
}

// Rolling back a cancel must not damage a body it never changed.
//
// PushPullOp saves each target's SHAPE before its boolean but its face lineage
// only after the boolean succeeds. undo() then restores every saved shape
// through Document::updateBody, which erases the face-id map and the
// generation ledger by design. So a target cancelled mid-boolean gets its
// geometry put back and its topology metadata destroyed - a change with no
// history entry behind it, on a body the operation never actually modified.
TEST(OpProgress, CancelDoesNotStripLineageFromAnUntouchedBody) {
    const TopoDS_Shape base = plate(14, 12);
    Document doc;
    const int id = doc.addBody(base, "plate");

    // Give the body the kind of lineage an upstream operation would have left.
    materializr::topo::FaceIdMap lineage;
    int next = 1;
    for (TopExp_Explorer e(doc.getBody(id), TopAbs_FACE); e.More(); e.Next())
        materializr::topo::addId(lineage, e.Current(), next++);
    ASSERT_GT(lineage.size(), 0u);
    doc.setBodyFaceIds(id, lineage);
    const std::size_t before = doc.bodyFaceIds(id) ? doc.bodyFaceIds(id)->size() : 0;
    ASSERT_GT(before, 0u);

    auto reporter = std::make_shared<Reporter>();
    reporter->cancelAfter = 1;

    PushPullOp op;
    PushPullOp::Target t;
    t.profile = topFace(base);
    t.sourceBodyId = id;
    op.setTargets({t});
    op.setDistance(5.0);
    op.setProgressReporter(
        [reporter](float f, const char* l) { return (*reporter)(f, l); });

    EXPECT_FALSE(op.execute(doc));
    EXPECT_KERNEL_DROVE_IT(reporter);
    const std::size_t after = doc.bodyFaceIds(id) ? doc.bodyFaceIds(id)->size() : 0;
    EXPECT_EQ(after, before)
        << "the cancelled operation dropped the body's face lineage; it was "
        << before << " entries and is now " << after;
}

// No reporter set is the headless and preview-worker case: the bridge is
// never built, no range reaches the kernel, and the operation behaves exactly
// as it did before any of this existed.
TEST(OpProgress, NoReporterMeansNoBehaviourChange) {
    const TopoDS_Shape base = plate(6, 5);
    Document doc;
    const int id = doc.addBody(base, "plate");
    const double vol0 = volumeOf(doc.getBody(id));

    PushPullOp op;
    PushPullOp::Target t;
    t.profile = topFace(base);
    t.sourceBodyId = id;
    op.setTargets({t});
    op.setDistance(5.0);

    EXPECT_TRUE(op.execute(doc));
    EXPECT_GT(volumeOf(doc.getBody(id)), vol0);
}

// The throttle must not swallow the first callback: a window that only opens
// after 50 ms of geometry is a window the user never sees on a fast body, and
// a cancel latched before the first Show would never reach the kernel.
TEST(OpProgress, BridgeForwardsTheFirstCallAndLatchesCancel) {
    int calls = 0;
    materializr::OpProgressBridge* raw = new materializr::OpProgressBridge(
        [&calls](float, const char*) { ++calls; return true; }, "test");
    Handle(materializr::OpProgressBridge) bridge = raw;

    EXPECT_FALSE(bridge->UserBreak());
    {
        Message_ProgressScope scope(bridge->Start(), nullptr, 4);
        scope.Next();   // drives Show through the indicator
    }
    EXPECT_GT(calls, 0) << "the first callback was throttled away";
    EXPECT_TRUE(raw->cancelled());
    EXPECT_TRUE(bridge->UserBreak()) << "the cancel latch must be sticky";

    const int after = calls;
    {
        Message_ProgressScope scope(bridge->Start(), nullptr, 4);
        scope.Next();
    }
    EXPECT_EQ(calls, after) << "a cancelled bridge must stop calling the sink";
}
