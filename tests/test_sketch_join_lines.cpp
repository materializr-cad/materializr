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
