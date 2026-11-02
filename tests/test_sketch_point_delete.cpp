// Deleting a sketch point used to erase only the point: the lines, arcs and
// circles anchored on it stayed behind pointing at an id that no longer
// existed. They drop out of every profile, so a loop that still LOOKED closed
// no longer closed, and the body built from the sketch stopped following it
// (a reported lathe that vanished after one corner point was deleted).

#include "modeling/Sketch.h"

#include <gtest/gtest.h>
#include <unordered_set>

using namespace materializr;

namespace {

// Every element must reference only points that exist.
void expectNoDanglingReferences(const Sketch& sk) {
    std::unordered_set<int> pts;
    for (const auto& p : sk.getPoints()) pts.insert(p.id);
    for (const auto& l : sk.getLines()) {
        EXPECT_TRUE(pts.count(l.startPointId)) << "line " << l.id << " start";
        EXPECT_TRUE(pts.count(l.endPointId)) << "line " << l.id << " end";
    }
    for (const auto& c : sk.getCircles())
        EXPECT_TRUE(pts.count(c.centerPointId)) << "circle " << c.id;
    for (const auto& a : sk.getArcs()) {
        EXPECT_TRUE(pts.count(a.centerPointId)) << "arc " << a.id << " centre";
        EXPECT_TRUE(pts.count(a.startPointId)) << "arc " << a.id << " start";
        EXPECT_TRUE(pts.count(a.endPointId)) << "arc " << a.id << " end";
    }
    for (const auto& s : sk.getSplines())
        for (int id : s.controlPointIds)
            EXPECT_TRUE(pts.count(id)) << "spline " << s.id;
}

} // namespace

TEST(SketchPointDelete, DeletingACornerTakesBothLinesWithIt) {
    Sketch sk;
    int a = sk.addPoint({0, 0}), b = sk.addPoint({10, 0}), c = sk.addPoint({10, 10});
    sk.addLine(a, b);
    sk.addLine(b, c);
    sk.removeElements({b});
    EXPECT_TRUE(sk.getLines().empty());
    expectNoDanglingReferences(sk);
}

TEST(SketchPointDelete, OnlyLinesOnTheDeletedPointGo) {
    Sketch sk;
    int a = sk.addPoint({0, 0}), b = sk.addPoint({10, 0}), c = sk.addPoint({10, 10});
    int d = sk.addPoint({0, 10});
    sk.addLine(a, b);
    sk.addLine(b, c);
    const int keep = sk.addLine(c, d);
    sk.removeElements({a});
    ASSERT_EQ(sk.getLines().size(), 2u);
    bool kept = false;
    for (const auto& l : sk.getLines()) kept |= l.id == keep;
    EXPECT_TRUE(kept);
    expectNoDanglingReferences(sk);
}

TEST(SketchPointDelete, DeletingACircleCentreTakesTheCircle) {
    Sketch sk;
    int c = sk.addPoint({5, 5});
    sk.addCircle(c, 3.0);
    sk.removeElements({c});
    EXPECT_TRUE(sk.getCircles().empty());
    expectNoDanglingReferences(sk);
}

TEST(SketchPointDelete, SplineLosesTheControlPointButSurvives) {
    Sketch sk;
    int p0 = sk.addPoint({0, 0}), p1 = sk.addPoint({5, 5});
    int p2 = sk.addPoint({10, 0}), p3 = sk.addPoint({15, 5});
    sk.addSpline({p0, p1, p2, p3});
    sk.removeElements({p1});
    ASSERT_EQ(sk.getSplines().size(), 1u);
    EXPECT_EQ(sk.getSplines()[0].controlPointIds.size(), 3u);
    expectNoDanglingReferences(sk);
}

TEST(SketchPointDelete, SplineDownToOnePointIsRemoved) {
    Sketch sk;
    int p0 = sk.addPoint({0, 0}), p1 = sk.addPoint({5, 5});
    sk.addSpline({p0, p1});
    sk.removeElements({p1});
    EXPECT_TRUE(sk.getSplines().empty());
    expectNoDanglingReferences(sk);
}

TEST(SketchPointDelete, DeletingALineStillLeavesItsPointsForPruning) {
    Sketch sk;
    int a = sk.addPoint({0, 0}), b = sk.addPoint({10, 0});
    const int l = sk.addLine(a, b);
    sk.removeElements({l});
    EXPECT_TRUE(sk.getLines().empty());
    EXPECT_EQ(sk.getPoints().size(), 2u) << "endpoints are swept by pruneOrphanPoints";
    EXPECT_EQ(sk.pruneOrphanPoints(), 2);
}
