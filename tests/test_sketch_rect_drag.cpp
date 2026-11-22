// Dragging a vertex of a rotated rectangle, or of a quadrilateral held by two
// Parallel constraints, must not tear the shape apart (#187, build 865 report).
//
// Two faults in the solver's Parallel/Perpendicular corrections caused it:
//  - they forced the second line onto +dirA / +90 degrees even though the error
//    term is sign-blind, so the opposite sides of a rectangle (which run in
//    opposite directions round the loop) were flipped end for end the moment a
//    drag nudged them off exact;
//  - they moved the second line's END point even when that point is also an
//    end of the first line, so the direction being chased moved with the
//    correction and each pass overshot by the ratio of the side lengths.

#include "modeling/Sketch.h"
#include "modeling/SketchSolver.h"

#include <gtest/gtest.h>
#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>

using materializr::Constraint;
using materializr::ConstraintType;
using materializr::Sketch;
using materializr::SketchSolver;

namespace {

void addC(Sketch& sk, ConstraintType t, int a, int b = -1, double v = 0.0) {
    Constraint c{};
    c.type = t; c.entityA = a; c.entityB = b; c.value = v;
    sk.addConstraint(c);
}

glm::vec2 at(const Sketch& sk, int id) { return sk.getPoint(id)->pos; }

struct Rect {
    Sketch sk;
    int p[4];
    int l[4];
    double w, h;
};

// The Rectangle tool's output: four sides pinned H/V plus two dimensions,
// wound the way the tool winds them (+x+y, -x+y, -x-y, +x-y).
void build(Rect& r, double w, double h) {
    r.w = w; r.h = h;
    r.p[0] = r.sk.addPoint({float(w / 2),  float(h / 2)});
    r.p[1] = r.sk.addPoint({float(-w / 2), float(h / 2)});
    r.p[2] = r.sk.addPoint({float(-w / 2), float(-h / 2)});
    r.p[3] = r.sk.addPoint({float(w / 2),  float(-h / 2)});
    for (int i = 0; i < 4; ++i) r.l[i] = r.sk.addLine(r.p[i], r.p[(i + 1) % 4]);
    addC(r.sk, ConstraintType::Horizontal, r.l[0]);
    addC(r.sk, ConstraintType::Vertical,   r.l[1]);
    addC(r.sk, ConstraintType::Horizontal, r.l[2]);
    addC(r.sk, ConstraintType::Vertical,   r.l[3]);
    addC(r.sk, ConstraintType::Distance, r.p[0], r.p[1], w);
    addC(r.sk, ConstraintType::Distance, r.p[1], r.p[2], h);
}

void rotate(Rect& r, double deg) {
    std::vector<std::pair<int, glm::vec2>> orig;
    for (int id : r.p) orig.push_back({id, at(r.sk, id)});
    const auto base = r.sk.getConstraints();
    r.sk.rotatePointsAbout(orig, {0.0f, 0.0f}, deg * M_PI / 180.0, base);
    SketchSolver solver;
    solver.solve(r.sk);
}

// What the selection tool does each mouse move: put the grabbed point where
// the cursor is, then solve.
void dragTo(Sketch& sk, int pid, glm::vec2 target, int steps) {
    SketchSolver solver;
    const glm::vec2 from = at(sk, pid);
    for (int i = 1; i <= steps; ++i) {
        const float t = float(i) / float(steps);
        sk.movePoint(pid, from + (target - from) * t);
        solver.solve(sk);
    }
}

float maxCoord(const Sketch& sk) {
    float m = 0.0f;
    for (const auto& pt : sk.getPoints())
        m = std::max({m, std::fabs(pt.pos.x), std::fabs(pt.pos.y)});
    return m;
}

void expectRect(const Rect& r, float tol, const char* what) {
    SCOPED_TRACE(what);
    EXPECT_NEAR(glm::length(at(r.sk, r.p[1]) - at(r.sk, r.p[0])), r.w, tol);
    EXPECT_NEAR(glm::length(at(r.sk, r.p[2]) - at(r.sk, r.p[1])), r.h, tol);
    EXPECT_NEAR(glm::length(at(r.sk, r.p[3]) - at(r.sk, r.p[2])), r.w, tol);
    EXPECT_NEAR(glm::length(at(r.sk, r.p[0]) - at(r.sk, r.p[3])), r.h, tol);
    glm::vec2 a = glm::normalize(at(r.sk, r.p[1]) - at(r.sk, r.p[0]));
    glm::vec2 b = glm::normalize(at(r.sk, r.p[2]) - at(r.sk, r.p[1]));
    EXPECT_NEAR(glm::dot(a, b), 0.0f, 0.01f);
}

} // namespace

// The tester's file b865_rect1: a 4 x 8 rectangle, rotated off the axes, then
// dragged by a vertex. It turned into a stretched parallelogram that spun.
TEST(SketchRectDrag, RotatedRectangleSurvivesAVertexDrag) {
    for (double deg : {15.0, 37.0, 75.0, 120.0}) {
        for (auto wh : {std::pair<double, double>{4.0, 8.0}, {8.0, 4.0}, {16.0, 16.0}}) {
            Rect r; build(r, wh.first, wh.second);
            rotate(r, deg);
            for (int v = 0; v < 4; ++v) {
                Rect c = r;
                const glm::vec2 start = at(c.sk, c.p[v]);
                dragTo(c.sk, c.p[v], start + glm::vec2(25.0f, -18.0f), 30);
                EXPECT_LT(maxCoord(c.sk), 200.0f) << "deg " << deg << " vertex " << v;
                expectRect(c, 0.05f, "after drag");
            }
        }
    }
}

// The same rectangle dragged round in a circle (the tester saw it spin).
TEST(SketchRectDrag, DraggingARotatedRectangleAroundDoesNotSpinIt) {
    Rect r; build(r, 4.0, 8.0);
    rotate(r, 30.0);
    const glm::vec2 c0 = (at(r.sk, r.p[0]) + at(r.sk, r.p[2])) * 0.5f;
    for (int k = 0; k < 40; ++k) {
        const float a = float(k) * 0.3f;
        dragTo(r.sk, r.p[0], at(r.sk, r.p[0]) + glm::vec2(std::cos(a), std::sin(a)) * 3.0f, 4);
        ASSERT_LT(maxCoord(r.sk), 300.0f) << "step " << k;
    }
    expectRect(r, 0.05f, "after circling");
    const glm::vec2 c1 = (at(r.sk, r.p[0]) + at(r.sk, r.p[2])) * 0.5f;
    EXPECT_LT(glm::length(c1 - c0), 60.0f);
}

// The tester's file b865_rect2: a quadrilateral held only by two Parallel
// constraints (opposite sides), dragged by one corner. It collapsed to a line.
TEST(SketchRectDrag, ParallelQuadSurvivesAVertexDrag) {
    Sketch sk;
    const int p3 = sk.addPoint({-5.65685f, 9.79796f});
    const int p4 = sk.addPoint({-9.79796f, -5.65685f});
    const int p5 = sk.addPoint({5.65685f, -9.79796f});
    const int p6 = sk.addPoint({9.79796f, 5.65685f});
    const int l7  = sk.addLine(p3, p4);
    const int l8  = sk.addLine(p4, p5);
    const int l9  = sk.addLine(p5, p6);
    const int l10 = sk.addLine(p6, p3);
    addC(sk, ConstraintType::Parallel, l8, l10);
    addC(sk, ConstraintType::Parallel, l7, l9);

    auto area = [&] {
        const glm::vec2 a = at(sk, p3), b = at(sk, p4), c = at(sk, p5), d = at(sk, p6);
        return 0.5f * std::fabs((a.x * b.y - b.x * a.y) + (b.x * c.y - c.x * b.y) +
                                (c.x * d.y - d.x * c.y) + (d.x * a.y - a.x * d.y));
    };
    ASSERT_GT(area(), 100.0f);

    dragTo(sk, p3, {29.9101f, -17.3112f}, 30);
    EXPECT_LT(maxCoord(sk), 200.0f);
    EXPECT_GT(area(), 50.0f) << "the quad collapsed";
    const glm::vec2 s7 = at(sk, p4) - at(sk, p3), s9 = at(sk, p6) - at(sk, p5);
    const glm::vec2 s8 = at(sk, p5) - at(sk, p4), s10 = at(sk, p3) - at(sk, p6);
    EXPECT_NEAR(s7.x * s9.y - s7.y * s9.x, 0.0f, 0.5f);
    EXPECT_NEAR(s8.x * s10.y - s8.y * s10.x, 0.0f, 0.5f);
}

// A perpendicular placed on a side that sits at -90 degrees to its reference
// must turn it the short way, not through half a turn.
TEST(SketchRectDrag, PerpendicularKeepsTheSideItIsOn) {
    Sketch sk;
    const int a = sk.addPoint({0.0f, 0.0f});
    const int b = sk.addPoint({10.0f, 0.0f});
    const int c = sk.addPoint({10.5f, -6.0f});   // B runs down from b: -90 from A
    const int lA = sk.addLine(a, b);
    const int lB = sk.addLine(b, c);
    addC(sk, ConstraintType::Perpendicular, lA, lB);
    SketchSolver solver;
    ASSERT_TRUE(solver.solve(sk));
    EXPECT_NEAR(at(sk, c).x, 10.0f, 0.05f);
    EXPECT_LT(at(sk, c).y, -5.0f) << "the side was flipped to the other side of A";
}

// Whatever drives it there, a solve that runs away must be thrown out rather
// than saved: the tester's project ended with points at 1e13 and no view of it
// worked.
TEST(SketchRectDrag, ARunawaySolveIsUndoneNotKept) {
    Sketch sk;
    const int a = sk.addPoint({0.0f, 0.0f});
    const int b = sk.addPoint({10.0f, 0.0f});
    addC(sk, ConstraintType::Distance, a, b, 1e12);   // asks for the impossible
    SketchSolver solver;
    EXPECT_FALSE(solver.solve(sk));
    EXPECT_LT(maxCoord(sk), 100.0f);
    EXPECT_NEAR(at(sk, a).x, 0.0f, 1e-3f);
    EXPECT_NEAR(at(sk, b).x, 10.0f, 1e-3f);
}
