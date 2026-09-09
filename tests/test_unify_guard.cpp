// UnifySameDomain is cosmetic, and it does not get a vote on whether an
// operation succeeded.
//
// The bug this pins (2026-08-20, "robot ass" project): a Union of two healthy
// solids fused perfectly - 227.049 = 219.691 + 7.357, one solid, valid - and
// then the seam-merge pass turned it into a 161.075 mm3 INVALID shape while
// merging nothing at all (9 faces in, 9 faces out). BooleanOp validity-checked
// the mangled shape, saw false, and reported "Fuse failed even with fuzzy" over
// a fuse that had succeeded. Union looked like a dead button.
//
// It is worse than it looks: UnifySameDomain edits its input IN PLACE, so the
// pre-unify shape is corrupted too and there is nothing left to fall back to
// unless a copy was taken first. materializr::unifySameDomain does exactly
// that - copy, merge, and hand back the copy when the merge moved geometry or
// broke validity.
// These tests cover the guard's contract and, just as importantly, that it
// still LETS the ordinary merge through - a guard that rejects everything
// would leave a seam on every boolean and nobody would notice for months.
#include <gtest/gtest.h>

#include "core/Document.h"
#include "modeling/BooleanOp.h"
#include "modeling/UnifyTolerance.h"

#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>

#include <cmath>

namespace {

int faceCount(const TopoDS_Shape& s) {
    TopTools_IndexedMapOfShape m;
    TopExp::MapShapes(s, TopAbs_FACE, m);
    return m.Extent();
}

double volumeOf(const TopoDS_Shape& s) {
    GProp_GProps g;
    BRepGProp::VolumeProperties(s, g);
    return g.Mass();
}

TopoDS_Shape box(double x, double y, double z, double dx, double dy, double dz) {
    return BRepPrimAPI_MakeBox(gp_Pnt(x, y, z), dx, dy, dz).Shape();
}

} // namespace

// A merge that changed nothing is adoptable.
TEST(UnifyGuard, AcceptsAnUnchangedShape) {
    const TopoDS_Shape b = box(0, 0, 0, 10, 10, 10);
    EXPECT_TRUE(materializr::unifyIsSafe(b, b, "test"));
}

// The failure that started this: same face count, different volume. A shape
// that passes BRepCheck can still be the wrong part.
TEST(UnifyGuard, RejectsAVolumeChange) {
    const TopoDS_Shape before = box(0, 0, 0, 10, 10, 10);   // 1000
    const TopoDS_Shape after  = box(0, 0, 0, 10, 10,  7);   //  700, still valid
    ASSERT_NEAR(volumeOf(before), 1000.0, 1e-6);
    ASSERT_NEAR(volumeOf(after),   700.0, 1e-6);
    EXPECT_FALSE(materializr::unifyIsSafe(before, after, "test"));
}

// Rounding noise from re-fitting a surface is not a reshape. The slack is
// relative (1e-4), matching MergeFacesOp's, so the two guards agree.
TEST(UnifyGuard, ToleratesRoundingNoise) {
    const TopoDS_Shape before = box(0, 0, 0, 10, 10, 10);
    const TopoDS_Shape after  = box(0, 0, 0, 10, 10, 10.000001);
    EXPECT_TRUE(materializr::unifyIsSafe(before, after, "test"));
}

TEST(UnifyGuard, RejectsANullResult) {
    const TopoDS_Shape before = box(0, 0, 0, 10, 10, 10);
    EXPECT_FALSE(materializr::unifyIsSafe(before, TopoDS_Shape(), "test"));
}

// A null shape in is a null shape out, not a crash.
TEST(UnifyGuard, PassesANullShapeThrough) {
    EXPECT_TRUE(materializr::unifySameDomain(TopoDS_Shape(), "test").IsNull());
}

// The wrapper on a shape with nothing to merge: unchanged, and still sound.
TEST(UnifyGuard, LeavesAPlainSolidAlone) {
    const TopoDS_Shape b = box(0, 0, 0, 10, 10, 10);
    const TopoDS_Shape r = materializr::unifySameDomain(b, "test");
    ASSERT_FALSE(r.IsNull());
    EXPECT_EQ(faceCount(r), 6);
    EXPECT_NEAR(volumeOf(r), 1000.0, 1e-6);
    EXPECT_TRUE(BRepCheck_Analyzer(r).IsValid());
}

// The wrapper does the merge it exists to do: a fused stack of two boxes comes
// back as one plain box.
TEST(UnifyGuard, MergesThroughTheWrapper) {
    const TopoDS_Shape fused =
        BRepAlgoAPI_Fuse(box(0, 0, 0, 10, 10, 10), box(0, 0, 10, 10, 10, 10)).Shape();
    // The fuse itself already drops the two coincident 10x10 faces; the four
    // coplanar wall pairs are what unify is here to merge.
    ASSERT_EQ(faceCount(fused), 10);
    const TopoDS_Shape r = materializr::unifySameDomain(fused, "test");
    EXPECT_EQ(faceCount(r), 6);
    EXPECT_NEAR(volumeOf(r), 2000.0, 1e-6);
}

// The guard must not cost us the merge it is guarding. Two boxes stacked face
// to face fuse into one solid where every boundary between them is redundant -
// the shared 10x10 face (which the fuse drops on its own) and the four coplanar
// side-wall pairs. Unify merges the walls and hands back a plain 10x10x20 box,
// and the guard has to let that through: 10 faces in, 6 out.
TEST(UnifyGuard, StillMergesTheSeamOnANormalUnion) {
    Document doc;
    const int target = doc.addBody(box(0, 0, 0, 10, 10, 10), "target");
    const int tool   = doc.addBody(box(0, 0, 10, 10, 10, 10), "tool");

    BooleanOp op;
    op.setTargetBodyId(target);
    op.setToolBodyId(tool);
    op.setMode(BooleanMode::Union);
    ASSERT_TRUE(op.execute(doc));

    const TopoDS_Shape r = doc.getBody(target);
    EXPECT_NEAR(volumeOf(r), 2000.0, 1e-6);
    // One box, not two stacked: the seam and the four wall splits are gone.
    EXPECT_EQ(faceCount(r), 6);
}

// An overlapping union: the arithmetic has to be the union's, not the sum's.
TEST(UnifyGuard, OverlappingUnionKeepsItsVolume) {
    Document doc;
    const int target = doc.addBody(box(0, 0, 0, 10, 10, 10), "target");
    const int tool   = doc.addBody(box(5, 0, 0, 10, 10, 10), "tool");

    BooleanOp op;
    op.setTargetBodyId(target);
    op.setToolBodyId(tool);
    op.setMode(BooleanMode::Union);
    ASSERT_TRUE(op.execute(doc));

    // 15 x 10 x 10 - the 5mm overlap counted once.
    EXPECT_NEAR(volumeOf(doc.getBody(target)), 1500.0, 1e-6);
}
