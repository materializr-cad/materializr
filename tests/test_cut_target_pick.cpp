// Subtract-from-sketch picks its victim by GEOMETRY, not by provenance.
//
// The old rule was "cut the body this sketch was drawn on, and if there isn't
// one, do nothing" - so the Subtract button was a silent no-op for every sketch
// on a construction or origin plane, and for every sketch that had been unlinked
// from its body. Worse, a linked sketch whose sweep MISSED its host still landed
// a History step: BRepAlgoAPI_Cut hands back the untouched body, which is valid,
// non-empty, and passes every guard in ExtrudeOp.
//
// So the target is whichever visible body the swept tool volume actually removes
// material from, with the host body preferred when it is one of them.
#include <gtest/gtest.h>

#include "core/Document.h"
#include "modeling/CutTargetPick.h"
#include "modeling/ExtrudeOp.h"
#include "modeling/Sketch.h"

#include <BRepGProp.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <GProp_GProps.hxx>
#include <gp_Ax3.hxx>
#include <gp_Dir.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt.hxx>

#include <memory>

using materializr::Sketch;
using materializr::cutpick::cutSweepSign;
using materializr::cutpick::pickAllCutTargets;
using materializr::cutpick::pickCutTarget;
using materializr::cutpick::removedVolume;

namespace {

// A box with its near-bottom-left corner at (x,y,z).
TopoDS_Shape box(double x, double y, double z, double dx, double dy, double dz) {
    return BRepPrimAPI_MakeBox(gp_Pnt(x, y, z), dx, dy, dz).Shape();
}

double volumeOf(const TopoDS_Shape& s) {
    GProp_GProps g;
    BRepGProp::VolumeProperties(s, g);
    return g.Mass();
}

// A square sketch on a plane at height z, facing +Z - a construction-plane
// sketch, with no source body and no source face.
std::shared_ptr<Sketch> floatingSquare(double z, double x0, double y0,
                                       double side) {
    auto sk = std::make_shared<Sketch>();
    sk->setPlane(gp_Pln(gp_Ax3(gp_Pnt(0, 0, z), gp_Dir(0, 0, 1),
                               gp_Dir(1, 0, 0))));
    const float a = static_cast<float>(x0), b = static_cast<float>(y0);
    const float s = static_cast<float>(side);
    int p[4] = { sk->addPoint({a, b}),     sk->addPoint({a + s, b}),
                 sk->addPoint({a + s, b + s}), sk->addPoint({a, b + s}) };
    for (int i = 0; i < 4; ++i) sk->addLine(p[i], p[(i + 1) % 4]);
    return sk;
}

} // namespace

TEST(CutTargetPick, TheToolCutsTheBodyItOverlaps) {
    // Two bodies side by side; the tool sits over the second one only.
    std::vector<std::pair<int, TopoDS_Shape>> bodies{
        {7, box(0, 0, 0, 10, 10, 10)},
        {9, box(20, 0, 0, 10, 10, 10)},
    };
    TopoDS_Shape tool = box(22, 2, -1, 4, 4, 12);
    EXPECT_EQ(pickCutTarget(bodies, tool), 9);
}

TEST(CutTargetPick, AToolThatReachesNothingPicksNothing) {
    // The whole point: this used to be indistinguishable from a successful cut,
    // because Cut against a body it misses returns that body unchanged.
    std::vector<std::pair<int, TopoDS_Shape>> bodies{
        {7, box(0, 0, 0, 10, 10, 10)},
    };
    TopoDS_Shape tool = box(100, 100, 100, 4, 4, 4);
    EXPECT_EQ(pickCutTarget(bodies, tool), -1);
}

TEST(CutTargetPick, StraddlingTwoBodiesTheDeeperCutWins) {
    // 8 mm of the tool lies in body 1, 2 mm in body 2.
    std::vector<std::pair<int, TopoDS_Shape>> bodies{
        {1, box(0, 0, 0, 8, 10, 10)},
        {2, box(8, 0, 0, 10, 10, 10)},
    };
    TopoDS_Shape tool = box(0, 2, -1, 10, 4, 12);
    EXPECT_EQ(pickCutTarget(bodies, tool), 1);
}

TEST(CutTargetPick, TheHostBodyWinsEvenWhenItIsNotTheDeeperCut) {
    // A sketch drawn on body 2's face keeps cutting body 2, even where its
    // profile also clips a neighbour it happens to overlap more of.
    std::vector<std::pair<int, TopoDS_Shape>> bodies{
        {1, box(0, 0, 0, 8, 10, 10)},
        {2, box(8, 0, 0, 10, 10, 10)},
    };
    TopoDS_Shape tool = box(0, 2, -1, 10, 4, 12);
    EXPECT_EQ(pickCutTarget(bodies, tool, /*preferred=*/2), 2);
}

TEST(CutTargetPick, AHostTheSweepMissesFallsBackToWhatItHits) {
    // The link is stale or the sketch has been moved away: prefer the host, but
    // never let a preference the tool never touches veto a real cut.
    std::vector<std::pair<int, TopoDS_Shape>> bodies{
        {1, box(0, 0, 0, 10, 10, 10)},
        {2, box(50, 0, 0, 10, 10, 10)},
    };
    TopoDS_Shape tool = box(2, 2, -1, 4, 4, 12);
    EXPECT_EQ(pickCutTarget(bodies, tool, /*preferred=*/2), 1);
}

TEST(CutTargetPick, BodiesMerelyTouchingTheToolAreNotCut) {
    // Face-to-face contact reports a sliver of "common" volume in OCCT. Treating
    // that as a cut would target a body the sweep only grazes.
    std::vector<std::pair<int, TopoDS_Shape>> bodies{
        {1, box(0, 0, 0, 10, 10, 10)},
    };
    TopoDS_Shape tool = box(10, 2, 2, 5, 4, 4);   // shares the x=10 face exactly
    EXPECT_EQ(pickCutTarget(bodies, tool), -1);
}

TEST(CutTargetPick, RemovedVolumeIsTheOverlap) {
    TopoDS_Shape body = box(0, 0, 0, 10, 10, 10);
    TopoDS_Shape tool = box(5, 0, 0, 10, 10, 10);   // half in, half out
    EXPECT_NEAR(removedVolume(body, tool), 500.0, 1e-6);
}

TEST(CutTargetPick, TheSweepAimsAtTheNearestBody) {
    // A sketch on a construction plane at z=20, normal +Z, with the part below
    // it. Sweeping along the normal would cut nothing but air.
    std::vector<TopoDS_Shape> bodies{box(0, 0, 0, 10, 10, 10)};
    EXPECT_DOUBLE_EQ(cutSweepSign(gp_Pnt(5, 5, 20), gp_Dir(0, 0, 1), bodies), -1.0);
    // Flip the plane's normal and the answer flips with it.
    EXPECT_DOUBLE_EQ(cutSweepSign(gp_Pnt(5, 5, 20), gp_Dir(0, 0, -1), bodies), 1.0);
}

TEST(CutTargetPick, TheSweepPrefersTheNearestBodyNotTheFurthest) {
    // Nearest by true distance, not by distance along the normal: the far body
    // is further along +Z than the near one is along -Z, so ranking on the
    // normal component alone would aim the cut at the wrong part.
    std::vector<TopoDS_Shape> bodies{
        box(0, 0, -14, 10, 10, 10),     // centre z = -9, 9 mm below
        box(0, 0, 40, 10, 10, 10),      // centre z = 45, 45 mm above
    };
    EXPECT_DOUBLE_EQ(cutSweepSign(gp_Pnt(5, 5, 0), gp_Dir(0, 0, 1), bodies), -1.0);
}

TEST(CutTargetPick, NothingToAimAtSweepsAlongTheNormal) {
    EXPECT_DOUBLE_EQ(cutSweepSign(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1), {}), 1.0);
}

// ─── End to end: the case that was a no-op ───────────────────────────────────
// A sketch on a construction plane floating above a part. It has no source body,
// so the old Subtract path printed to stderr and returned before anything was
// built. Here the whole chain runs: aim the sweep, build the tool volume, find
// what it hits, cut it.
TEST(CutTargetPick, AFloatingSketchCutsThePartBelowIt) {
    Document doc;
    const int part = doc.addBody(box(0, 0, 0, 20, 20, 10), "part");
    const double before = volumeOf(doc.getBody(part));

    auto sk = floatingSquare(/*z=*/20.0, /*x0=*/6.0, /*y0=*/6.0, /*side=*/4.0);
    const int sid = doc.addSketch(sk);
    ASSERT_EQ(sk->getSourceBody(), -1) << "this sketch has no host body";

    // The plane's normal is +Z and the part is BELOW it, so the cut must run the
    // other way. Sweeping along the normal would carve nothing but air - which
    // is what a fixed "Subtract goes against the normal" rule gives you here
    // only by luck, and gets backwards on a plane the user flipped.
    std::vector<TopoDS_Shape> shapes{doc.getBody(part)};
    const double sign = cutSweepSign(gp_Pnt(0, 0, 20), gp_Dir(0, 0, 1), shapes);
    ASSERT_DOUBLE_EQ(sign, -1.0);

    // The tool volume, exactly as the live preview builds it: a NewBody extrude
    // at the signed distance.
    ExtrudeOp tool;
    tool.setSketchSource(sid);
    tool.setDistance(sign * 22.0);          // z=20 down to z=-2: clean through
    tool.setMode(ExtrudeMode::NewBody);
    ASSERT_TRUE(tool.rebuildProfileFromSketch(doc));
    ASSERT_TRUE(tool.execute(doc));
    const int toolId = tool.createdBodyId();
    ASSERT_GE(toolId, 0);

    std::vector<std::pair<int, TopoDS_Shape>> bodies{{part, doc.getBody(part)}};
    EXPECT_EQ(pickCutTarget(bodies, doc.getBody(toolId), /*preferred=*/-1), part);

    // And the cut itself lands: a 4x4 hole through the full 10 mm of plate.
    ASSERT_TRUE(tool.undo(doc));            // drop the preview volume
    ExtrudeOp cut;
    cut.setSketchSource(sid);
    cut.setDistance(sign * 22.0);
    cut.setMode(ExtrudeMode::Subtract);
    cut.setTargetBody(part);
    ASSERT_TRUE(cut.rebuildProfileFromSketch(doc));
    ASSERT_TRUE(cut.execute(doc));
    EXPECT_NEAR(volumeOf(doc.getBody(part)), before - 4.0 * 4.0 * 10.0, 1e-6);
}

TEST(CutTargetPick, ASweepThatStopsShortReachesNothing) {
    // The other silent failure: a target IS known, but the tool never gets
    // there. BRepAlgoAPI_Cut returns the body untouched - valid, non-empty, and
    // indistinguishable from a real cut once it is on History. Catching it needs
    // exactly this overlap test.
    Document doc;
    const int part = doc.addBody(box(0, 0, 0, 20, 20, 10), "part");
    auto sk = floatingSquare(/*z=*/20.0, 6.0, 6.0, 4.0);
    const int sid = doc.addSketch(sk);

    ExtrudeOp tool;
    tool.setSketchSource(sid);
    tool.setDistance(-5.0);                 // z=20 down to z=15 - stops above
    tool.setMode(ExtrudeMode::NewBody);
    ASSERT_TRUE(tool.rebuildProfileFromSketch(doc));
    ASSERT_TRUE(tool.execute(doc));

    std::vector<std::pair<int, TopoDS_Shape>> bodies{{part, doc.getBody(part)}};
    EXPECT_EQ(pickCutTarget(bodies, doc.getBody(tool.createdBodyId()),
                            /*preferred=*/part), -1)
        << "a preferred target the sweep never reaches is still no target";
}

// ─── The all-bodies option ───────────────────────────────────────────────────
TEST(CutTargetPick, CuttingEverythingReturnsEveryBodyTheSweepReaches) {
    // A profile driven through a stack: the one-target rule cuts the first
    // plate and the rest of the sweep vanishes into bodies it visibly passes
    // through. With the option on, every one of them is a target.
    std::vector<std::pair<int, TopoDS_Shape>> bodies{
        {3, box(0, 0, 0,  10, 10, 4)},     // z 0..4
        {1, box(0, 0, 6,  10, 10, 4)},     // z 6..10
        {2, box(0, 0, 12, 10, 10, 4)},     // z 12..16
        {5, box(40, 0, 0, 10, 10, 4)},     // off to the side - untouched
    };
    TopoDS_Shape tool = box(3, 3, -1, 3, 3, 20);
    const std::vector<int> hits = pickAllCutTargets(bodies, tool);
    // Sorted by id, NOT by the order the bodies happened to arrive in: the
    // caller pushes one History step each, and replay order must be stable.
    EXPECT_EQ(hits, (std::vector<int>{1, 2, 3}));
}

TEST(CutTargetPick, CuttingEverythingStillSkipsWhatItOnlyTouches) {
    // Same sliver-of-common-volume trap as the single-target pick: a body the
    // tool merely shares a face with is not one the sweep passes through.
    std::vector<std::pair<int, TopoDS_Shape>> bodies{
        {1, box(0, 0, 0, 10, 10, 10)},
        {2, box(10, 0, 0, 10, 10, 10)},   // shares the x=10 face with body 1
    };
    TopoDS_Shape tool = box(2, 2, 2, 8, 4, 4);   // inside 1, up to the seam
    EXPECT_EQ(pickAllCutTargets(bodies, tool), (std::vector<int>{1}));
}

TEST(CutTargetPick, CuttingEverythingReachingNothingIsEmpty) {
    std::vector<std::pair<int, TopoDS_Shape>> bodies{
        {1, box(0, 0, 0, 10, 10, 10)},
    };
    EXPECT_TRUE(pickAllCutTargets(bodies, box(50, 50, 50, 2, 2, 2)).empty());
}
