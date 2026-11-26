// Deleting a point where two lines meet keeps the line (#190): both lines and the
// point go, and one line runs between their far ends. Until now deleting the point
// deleted the lines with it.
#include <gtest/gtest.h>

#include "modeling/Sketch.h"

#include <glm/glm.hpp>

using materializr::Sketch;

namespace {
bool hasLineBetween(const Sketch& sk, glm::vec2 p, glm::vec2 q) {
    for (const auto& l : sk.getLines()) {
        const auto* a = sk.getPoint(l.startPointId);
        const auto* b = sk.getPoint(l.endPointId);
        if (!a || !b) continue;
        const bool fwd = glm::length(a->pos - p) < 1e-4f && glm::length(b->pos - q) < 1e-4f;
        const bool bwd = glm::length(a->pos - q) < 1e-4f && glm::length(b->pos - p) < 1e-4f;
        if (fwd || bwd) return true;
    }
    return false;
}
}

TEST(SketchJoinLines, CollinearLinesBecomeOneContinuousLine) {
    Sketch sk;
    const int a = sk.addPoint({0, 0}), m = sk.addPoint({5, 0}), b = sk.addPoint({10, 0});
    sk.addLine(a, m);
    sk.addLine(m, b);

    int made = -1;
    ASSERT_TRUE(sk.joinLinesAt(m, &made));
    EXPECT_EQ(sk.getPoint(m), nullptr) << "the point itself is gone";
    ASSERT_EQ(sk.getLines().size(), 1u);
    EXPECT_EQ(sk.getLines().front().id, made);
    EXPECT_TRUE(hasLineBetween(sk, {0, 0}, {10, 0})) << "extreme points keep their coordinates";
}

TEST(SketchJoinLines, LinesAtAnAngleBecomeAShortcut) {
    Sketch sk;
    const int a = sk.addPoint({0, 0}), m = sk.addPoint({5, 5}), b = sk.addPoint({10, 0});
    sk.addLine(a, m);
    sk.addLine(m, b);

    ASSERT_TRUE(sk.joinLinesAt(m));
    ASSERT_EQ(sk.getLines().size(), 1u);
    EXPECT_TRUE(hasLineBetween(sk, {0, 0}, {10, 0}));
}

TEST(SketchJoinLines, ASquareCornerLeavesAClosedTriangle) {
    Sketch sk;
    sk.addRectangle({0, 0}, {10, 10});
    int corner = -1;
    for (const auto& p : sk.getPoints())
        if (glm::length(p.pos) < 1e-6f) corner = p.id;
    ASSERT_GE(corner, 0);

    ASSERT_TRUE(sk.joinLinesAt(corner));
    EXPECT_EQ(sk.getLines().size(), 4u - 1u);
    EXPECT_EQ(sk.buildRegions().size(), 1u) << "still one closed contour";
}

TEST(SketchJoinLines, ChainsLoseSeveralPointsInAnyOrder) {
    Sketch sk;
    const int p1 = sk.addPoint({0, 0}), p2 = sk.addPoint({2, 1}),
              p3 = sk.addPoint({4, -1}), p4 = sk.addPoint({6, 0});
    sk.addLine(p1, p2);
    sk.addLine(p2, p3);
    sk.addLine(p3, p4);

    ASSERT_TRUE(sk.joinLinesAt(p2));
    ASSERT_TRUE(sk.joinLinesAt(p3));
    ASSERT_EQ(sk.getLines().size(), 1u);
    EXPECT_TRUE(hasLineBetween(sk, {0, 0}, {6, 0}));
}

TEST(SketchJoinLines, RefusesWhatItCannotJoinAndChangesNothing) {
    {   // A T junction: three lines meet.
        Sketch sk;
        const int c = sk.addPoint({0, 0});
        sk.addLine(c, sk.addPoint({5, 0}));
        sk.addLine(c, sk.addPoint({-5, 0}));
        sk.addLine(c, sk.addPoint({0, 5}));
        EXPECT_FALSE(sk.joinLinesAt(c));
        EXPECT_EQ(sk.getLines().size(), 3u);
    }
    {   // A lone end of a line.
        Sketch sk;
        const int a = sk.addPoint({0, 0});
        sk.addLine(a, sk.addPoint({5, 0}));
        EXPECT_FALSE(sk.joinLinesAt(a));
        EXPECT_EQ(sk.getLines().size(), 1u);
    }
    {   // A triangle: the shortcut already exists.
        Sketch sk;
        const int a = sk.addPoint({0, 0}), b = sk.addPoint({6, 0}), c = sk.addPoint({3, 5});
        sk.addLine(a, b);
        sk.addLine(b, c);
        sk.addLine(c, a);
        EXPECT_FALSE(sk.joinLinesAt(b));
        EXPECT_EQ(sk.getLines().size(), 3u);
        EXPECT_NE(sk.getPoint(b), nullptr);
    }
    {   // A polygon's vertex: the polygon keeps its own delete rules.
        Sketch sk;
        const int ctr = sk.addPoint({0, 0});
        sk.addPolygon(ctr, 5.0, 6);
        ASSERT_FALSE(sk.getPolygons().empty());
        const int v = sk.getPolygons().front().vertexPointIds.front();
        const size_t before = sk.getLines().size();
        EXPECT_FALSE(sk.joinLinesAt(v));
        EXPECT_EQ(sk.getLines().size(), before);
    }
}

TEST(SketchJoinLines, ConstructionOnlyWhenBothWere) {
    Sketch sk;
    const int a = sk.addPoint({0, 0}), m = sk.addPoint({5, 0}), b = sk.addPoint({10, 0});
    const int l0 = sk.addLine(a, m);
    sk.addLine(m, b);
    sk.setElementConstruction(l0, true);

    int made = -1;
    ASSERT_TRUE(sk.joinLinesAt(m, &made));
    EXPECT_FALSE(sk.isConstructionElement(made)) << "one real line keeps the join real";
}

// --- Two points at one spot are one corner (#203) -------------------------------------

namespace {
// The sketch from the report: an outline whose last line ends on a SECOND point at
// (6, 6), the position of an existing corner, with nothing joining the two.
struct Reported {
    Sketch sk;
    int corner = -1, strayEnd = -1;
};
Reported reportedSketch() {
    Reported r;
    Sketch& sk = r.sk;
    const int p1 = sk.addPoint({0, 0}),   p2 = sk.addPoint({6, -0.005f});
    r.corner  = sk.addPoint({6, 6});
    const int p6 = sk.addPoint({0, 4}),   p8 = sk.addPoint({-8, 4}),
              p10 = sk.addPoint({-8, 3}), p12 = sk.addPoint({-10, 3}),
              p14 = sk.addPoint({-10, 6});
    r.strayEnd = sk.addPoint({6, 6});
    sk.addLine(p1, p2);   sk.addLine(p2, r.corner); sk.addLine(p1, p6);
    sk.addLine(p6, p8);   sk.addLine(p8, p10);      sk.addLine(p10, p12);
    sk.addLine(p12, p14); sk.addLine(p14, r.strayEnd);
    return r;
}
}

TEST(SketchMergePoints, ACornerMadeOfTwoCoincidentPointsLeavesTheContourOpen) {
    Reported r = reportedSketch();
    EXPECT_EQ(r.sk.buildRegions().size(), 0u) << "looks closed, is two points";

    ASSERT_TRUE(r.sk.mergePointInto(r.strayEnd, r.corner));
    EXPECT_EQ(r.sk.getPoint(r.strayEnd), nullptr);
    EXPECT_NE(r.sk.getPoint(r.corner), nullptr);
    EXPECT_EQ(r.sk.getLines().size(), 8u) << "no line is lost";
    EXPECT_EQ(r.sk.buildRegions().size(), 1u) << "and the outline is closed now";
}

TEST(SketchMergePoints, EveryReferenceFollowsTheKeptPoint) {
    Sketch sk;
    const int a = sk.addPoint({0, 0}), b = sk.addPoint({5, 5}), c = sk.addPoint({5, 5}),
              d = sk.addPoint({9, 0});
    sk.addLine(a, b);
    sk.addLine(c, d);
    const int ctr = sk.addPoint({5, 5});
    const int circ = sk.addCircle(ctr, 1.0);

    ASSERT_TRUE(sk.mergePointInto(c, b));
    for (const auto& l : sk.getLines()) {
        EXPECT_NE(l.startPointId, c);
        EXPECT_NE(l.endPointId, c);
    }
    // (the circle's own centre is a separate point and is untouched)
    for (const auto& k : sk.getCircles()) if (k.id == circ) EXPECT_EQ(k.centerPointId, ctr);
}

TEST(SketchMergePoints, RefusesWhatWouldCollapseOrBreakSomething) {
    {   // The two ends of one line: merging would delete the line.
        Sketch sk;
        const int a = sk.addPoint({0, 0}), b = sk.addPoint({0, 0});
        sk.addLine(a, b);
        EXPECT_FALSE(sk.mergePointInto(a, b));
        EXPECT_EQ(sk.getLines().size(), 1u);
        EXPECT_NE(sk.getPoint(a), nullptr);
    }
    {   // A polygon's vertex.
        Sketch sk;
        const int ctr = sk.addPoint({0, 0});
        sk.addPolygon(ctr, 5.0, 6);
        const int v = sk.getPolygons().front().vertexPointIds.front();
        const int other = sk.addPoint(sk.getPoint(v)->pos);
        EXPECT_FALSE(sk.mergePointInto(other, v));
        EXPECT_NE(sk.getPoint(other), nullptr);
    }
    {   // An id that does not exist, or the same point twice.
        Sketch sk;
        const int a = sk.addPoint({1, 1});
        EXPECT_FALSE(sk.mergePointInto(a, a));
        EXPECT_FALSE(sk.mergePointInto(a, 999));
    }
}

TEST(SketchMergePoints, LinesThatEndUpDoubledAreOneLine) {
    Sketch sk;
    const int a = sk.addPoint({0, 0}), b = sk.addPoint({4, 0}), b2 = sk.addPoint({4, 0});
    sk.addLine(a, b);
    sk.addLine(a, b2);   // two lines to what is the same place
    ASSERT_TRUE(sk.mergePointInto(b2, b));
    EXPECT_EQ(sk.getLines().size(), 1u);
}
