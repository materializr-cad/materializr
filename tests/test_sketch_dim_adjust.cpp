// Adjusting a dimension on an already-solved sketch must move the geometry to
// the new value without flipping it into a mirrored configuration.
//
// The solver is iterative relaxation over per-constraint corrections. Two of
// the correction branches derive a DIRECTION from the live geometry each pass,
// so a correction that drives the geometry through a degenerate configuration
// (coincident centres, a point crossing its reference line) comes out the far
// side pointing the other way and the sketch settles mirrored.

#include "modeling/Sketch.h"
#include "modeling/SketchSolver.h"

#include <gtest/gtest.h>
#include <glm/glm.hpp>
#include <cmath>

using materializr::Constraint;
using materializr::ConstraintType;
using materializr::Sketch;
using materializr::SketchSolver;

namespace {

// Which side of line (a→b) the point sits on. Sign is what must survive.
double sideOf(const Sketch& sk, int ptId, int lineId) {
    const auto* p = sk.getPoint(ptId);
    for (const auto& l : sk.getLines()) {
        if (l.id != lineId) continue;
        const auto* a = sk.getPoint(l.startPointId);
        const auto* b = sk.getPoint(l.endPointId);
        glm::vec2 d = b->pos - a->pos;
        glm::vec2 r = p->pos - a->pos;
        return static_cast<double>(d.x) * r.y - static_cast<double>(d.y) * r.x;
    }
    return 0.0;
}

} // namespace

// Two holes overlapping by more than their combined radii. A negative rim gap
// is a legal thing to dimension (intersecting bores), but the correction
// computes a TARGET CENTRE DISTANCE of value + rA + rB - which goes negative
// here. A distance cannot be negative, so the centres are driven through each
// other and the pair flips instead of settling.
TEST(DimAdjust, DeeplyNegativeCircleGapDoesNotFlipCentres) {
    Sketch sk;
    int ca = sk.addPoint({0.0f, 0.0f});
    int cb = sk.addPoint({20.0f, 0.0f});
    int a = sk.addCircle(ca, 5.0);
    int b = sk.addCircle(cb, 5.0);

    Constraint gap{};
    gap.type = ConstraintType::CircleGap;
    gap.entityA = a;
    gap.entityB = b;
    gap.value = 10.0;
    int gapId = sk.addConstraint(gap);

    SketchSolver solver;
    ASSERT_TRUE(solver.solve(sk, 500, 1e-4));

    // B started to the RIGHT of A. That ordering must survive the adjust.
    ASSERT_GT(sk.getPoint(cb)->pos.x, sk.getPoint(ca)->pos.x);

    // Achievable overlaps: the implied centre distance (value + rA + rB) is
    // still positive, so the pair just closes up and keeps its left-right
    // order.
    for (double v : {-5.0, -9.0}) {
        sk.movePoint(ca, {0.0f, 0.0f});
        sk.movePoint(cb, {20.0f, 0.0f});
        for (auto& c : sk.getMutableConstraints())
            if (c.id == gapId) c.value = v;

        EXPECT_TRUE(solver.solve(sk, 500, 1e-4)) << "gap=" << v;
        EXPECT_GT(sk.getPoint(cb)->pos.x, sk.getPoint(ca)->pos.x)
            << "gap=" << v << ": the circles swapped sides";
    }

    // Exactly -(rA + rB) is concentric - the deepest legal overlap, and a
    // perfectly good answer.
    sk.movePoint(ca, {0.0f, 0.0f});
    sk.movePoint(cb, {20.0f, 0.0f});
    for (auto& c : sk.getMutableConstraints())
        if (c.id == gapId) c.value = -10.0;
    EXPECT_TRUE(solver.solve(sk, 500, 1e-4));
    EXPECT_NEAR(glm::distance(sk.getPoint(ca)->pos, sk.getPoint(cb)->pos),
                0.0f, 1e-3f);

    // Below that the value is impossible. The solver must not thrash: it
    // settles deterministically at the closest achievable arrangement
    // (concentric) instead of oscillating for the whole iteration budget.
    for (double v : {-11.0, -15.0, -1000.0}) {
        sk.movePoint(ca, {0.0f, 0.0f});
        sk.movePoint(cb, {20.0f, 0.0f});
        for (auto& c : sk.getMutableConstraints())
            if (c.id == gapId) c.value = v;

        solver.solve(sk, 500, 1e-4);
        float ax = sk.getPoint(ca)->pos.x, bx = sk.getPoint(cb)->pos.x;
        ASSERT_TRUE(std::isfinite(ax) && std::isfinite(bx)) << "gap=" << v;
        EXPECT_NEAR(glm::distance(sk.getPoint(ca)->pos, sk.getPoint(cb)->pos),
                    0.0f, 1e-3f)
            << "gap=" << v << " should settle concentric, got ca.x=" << ax
            << " cb.x=" << bx;
    }
}

// Adjusting a Radius dimension on an ARC. The solver's radius setter used to
// write SketchArc::radius alone, leaving start and end where they were, so the
// stored radius disagreed with the radius the endpoints describe - and since
// buildWires places the arc's mid point from the stored radius, the emitted
// curve matched neither.
// This covers a standalone arc; the held-endpoint case is the test below.
TEST(DimAdjust, ArcRadiusAdjustKeepsEndpointsOnTheArc) {
    Sketch sk;
    int c = sk.addPoint({0.0f, 0.0f});
    int s = sk.addPoint({5.0f, 0.0f});
    int e = sk.addPoint({0.0f, 5.0f});
    int arc = sk.addArc(c, s, e, 5.0);

    Constraint r{};
    r.type = ConstraintType::Radius;
    r.entityA = arc;
    r.value = 5.0;
    int rId = sk.addConstraint(r);

    SketchSolver solver;
    ASSERT_TRUE(solver.solve(sk, 500, 1e-4));

    for (auto& cc : sk.getMutableConstraints())
        if (cc.id == rId) cc.value = 12.0;
    solver.solve(sk, 500, 1e-4);

    const auto* cp = sk.getPoint(c);
    double storedR = 0.0;
    for (const auto& a : sk.getArcs()) if (a.id == arc) storedR = a.radius;

    EXPECT_NEAR(storedR, 12.0, 1e-3);
    EXPECT_NEAR(glm::distance(sk.getPoint(s)->pos, cp->pos), storedR, 1e-3)
        << "start point still sits at the OLD radius";
    EXPECT_NEAR(glm::distance(sk.getPoint(e)->pos, cp->pos), storedR, 1e-3)
        << "end point still sits at the OLD radius";
}

// The case that made the endpoint sync a half-fix: an arc endpoint held by
// ANOTHER constraint. computeError for Radius used to read the cached field,
// so the setter ran once, the neighbour pulled the endpoint back off the
// radius, the cached number still matched the target, and solve() reported
// success over an arc that contradicted its own dimension. Measuring the
// endpoints means the solver either gets them there or honestly fails.
TEST(DimAdjust, ArcRadiusWithAHeldEndpointDoesNotFakeSuccess) {
    Sketch sk;
    int c = sk.addPoint({0.0f, 0.0f});
    int s = sk.addPoint({5.0f, 0.0f});
    int e = sk.addPoint({0.0f, 5.0f});
    int arc = sk.addArc(c, s, e, 5.0);

    Constraint r{};
    r.type = ConstraintType::Radius;
    r.entityA = arc;
    r.value = 5.0;
    int rId = sk.addConstraint(r);

    // Pin the start endpoint where it is. Growing the radius to 12 now
    // contradicts it: the arc cannot both pass through (5,0) and have radius
    // 12 about the origin.
    Constraint f{};
    f.type = ConstraintType::Fixed;
    f.entityA = s;
    f.value = 5.0;
    f.valueY = 0.0;
    sk.addConstraint(f);

    SketchSolver solver;
    ASSERT_TRUE(solver.solve(sk, 500, 1e-4));

    for (auto& cc : sk.getMutableConstraints())
        if (cc.id == rId) cc.value = 12.0;
    bool converged = solver.solve(sk, 500, 1e-4);

    const auto* cp = sk.getPoint(c);
    double ds = glm::distance(sk.getPoint(s)->pos, cp->pos);
    double storedR = 0.0;
    for (const auto& a : sk.getArcs()) if (a.id == arc) storedR = a.radius;

    // Fixed pulls the endpoint back to distance 5 every pass while Radius
    // measures the endpoints against 12: the system is contradictory, so the
    // contract is that the solver SAYS so rather than reporting success over
    // geometry that disagrees with its own dimension.
    EXPECT_FALSE(converged) << "claimed success on a contradictory system";
    bool radiusSatisfied = true;
    for (const auto& cc : sk.getConstraints())
        if (cc.id == rId) radiusSatisfied = cc.isSatisfied;
    EXPECT_FALSE(radiusSatisfied);
    EXPECT_TRUE(std::isfinite(ds) && std::isfinite(storedR));
    EXPECT_NEAR(sk.getPoint(s)->pos.x, 5.0f, 1e-3f) << "Fixed endpoint moved";
    EXPECT_NEAR(sk.getPoint(s)->pos.y, 0.0f, 1e-3f) << "Fixed endpoint moved";
}

// A point dimensioned to a line, then re-dimensioned to a much larger value.
// The point must stay on the side of the line the user placed it on.
TEST(DimAdjust, EnlargingPointLineDistanceKeepsSide) {
    Sketch sk;
    int a = sk.addPoint({0.0f, 0.0f});
    int b = sk.addPoint({10.0f, 0.0f});
    int ln = sk.addLine(a, b);
    int p = sk.addPoint({5.0f, 3.0f});

    Constraint d{};
    d.type = ConstraintType::DistancePointLine;
    d.entityA = p;
    d.entityB = ln;
    d.value = 3.0;
    int dId = sk.addConstraint(d);

    SketchSolver solver;
    ASSERT_TRUE(solver.solve(sk, 500, 1e-4));
    double before = sideOf(sk, p, ln);
    ASSERT_GT(before, 0.0);

    for (auto& c : sk.getMutableConstraints())
        if (c.id == dId) c.value = 30.0;

    solver.solve(sk, 500, 1e-4);
    EXPECT_GT(sideOf(sk, p, ln), 0.0) << "point crossed to the far side";
}

// Driving a point-line distance to zero and back out again: the point lands ON
// the line at the midpoint of the adjust, which is the degenerate state where
// the side information is lost.
TEST(DimAdjust, PointLineThroughZeroKeepsSide) {
    Sketch sk;
    int a = sk.addPoint({0.0f, 0.0f});
    int b = sk.addPoint({10.0f, 0.0f});
    int ln = sk.addLine(a, b);
    int p = sk.addPoint({5.0f, 4.0f});

    Constraint d{};
    d.type = ConstraintType::DistancePointLine;
    d.entityA = p;
    d.entityB = ln;
    d.value = 4.0;
    int dId = sk.addConstraint(d);

    SketchSolver solver;
    ASSERT_TRUE(solver.solve(sk, 500, 1e-4));
    ASSERT_GT(sideOf(sk, p, ln), 0.0);

    for (double v : {0.0, 6.0}) {
        for (auto& c : sk.getMutableConstraints())
            if (c.id == dId) c.value = v;
        solver.solve(sk, 500, 1e-4);
    }

    EXPECT_GT(sideOf(sk, p, ln), 0.0)
        << "passing through zero lost the side the dimension was placed on";
}

// A VERTICAL pair driven through zero. The degenerate guard used to invent a
// +x separation direction, so the pair came back horizontal - the dimension
// was satisfied and the sketch had silently rotated 90 degrees.
TEST(DimAdjust, PointPointDistanceThroughZeroKeepsAxis) {
    Sketch sk;
    int a = sk.addPoint({0.0f, 0.0f});
    int b = sk.addPoint({0.0f, 10.0f});

    Constraint d{};
    d.type = ConstraintType::Distance;
    d.entityA = a;
    d.entityB = b;
    d.value = 10.0;
    int id = sk.addConstraint(d);

    SketchSolver solver;
    ASSERT_TRUE(solver.solve(sk, 500, 1e-4));

    for (double v : {0.0, 10.0}) {
        for (auto& c : sk.getMutableConstraints())
            if (c.id == id) c.value = v;
        solver.solve(sk, 500, 1e-4);
    }

    glm::vec2 axis = sk.getPoint(b)->pos - sk.getPoint(a)->pos;
    EXPECT_GT(std::abs(axis.y), std::abs(axis.x))
        << "pair rotated off its vertical axis: (" << axis.x << "," << axis.y << ")";
    EXPECT_NEAR(glm::length(axis), 10.0f, 1e-3f);
}

// The mirror of PointLineThroughZeroKeepsSide. The original passed only
// because the point sat on the side the arbitrary normal happened to favour;
// placed on the other side it used to cross over.
TEST(DimAdjust, PointLineThroughZeroKeepsNegativeSide) {
    Sketch sk;
    int a = sk.addPoint({0.0f, 0.0f});
    int b = sk.addPoint({10.0f, 0.0f});
    int ln = sk.addLine(a, b);
    int p = sk.addPoint({5.0f, -4.0f});

    Constraint d{};
    d.type = ConstraintType::DistancePointLine;
    d.entityA = p;
    d.entityB = ln;
    d.value = 4.0;
    int id = sk.addConstraint(d);

    SketchSolver solver;
    ASSERT_TRUE(solver.solve(sk, 500, 1e-4));
    ASSERT_LT(sideOf(sk, p, ln), 0.0);

    for (double v : {0.0, 6.0}) {
        for (auto& c : sk.getMutableConstraints())
            if (c.id == id) c.value = v;
        solver.solve(sk, 500, 1e-4);
    }

    EXPECT_LT(sideOf(sk, p, ln), 0.0) << "point crossed to the far side";
    EXPECT_NEAR(std::abs(sk.getPoint(p)->pos.y), 6.0f, 1e-3f);
}

// A radius of zero or less is unreachable. Both setters clamp it to a hair
// above zero, and for an arc that clamp drags the endpoints in with it, so
// applying such a value would grind the arc onto its own centre and collapse
// whatever profile it belonged to. It must not apply at all.
TEST(DimAdjust, NonPositiveArcRadiusLeavesGeometryAlone) {
    Sketch sk;
    int c = sk.addPoint({0.0f, 0.0f});
    int s = sk.addPoint({5.0f, 0.0f});
    int e = sk.addPoint({0.0f, 5.0f});
    int arc = sk.addArc(c, s, e, 5.0);

    Constraint r{};
    r.type = ConstraintType::Radius;
    r.entityA = arc;
    r.value = 5.0;
    int id = sk.addConstraint(r);

    SketchSolver solver;
    ASSERT_TRUE(solver.solve(sk, 500, 1e-4));

    for (double v : {0.0, -3.0}) {
        for (auto& cc : sk.getMutableConstraints())
            if (cc.id == id) cc.value = v;
        solver.solve(sk, 500, 1e-4);

        const auto* cp = sk.getPoint(c);
        EXPECT_NEAR(glm::distance(sk.getPoint(s)->pos, cp->pos), 5.0f, 1e-3f)
            << "value=" << v << ": start endpoint collapsed onto the centre";
        EXPECT_NEAR(glm::distance(sk.getPoint(e)->pos, cp->pos), 5.0f, 1e-3f)
            << "value=" << v << ": end endpoint collapsed onto the centre";
    }
}

// The recorded side is only worth anything if the ERROR function enforces it.
// While the error was the unsigned |distance| - value, a point sitting on the
// wrong side at the right distance scored zero: no correction ran, the
// constraint reported satisfied, and the sketch sat mirrored until some
// unrelated edit disturbed the value and teleported it back across.
TEST(DimAdjust, PointOnWrongSideAtRightDistanceIsPulledBack) {
    Sketch sk;
    int a = sk.addPoint({0.0f, 0.0f});
    int b = sk.addPoint({10.0f, 0.0f});
    int ln = sk.addLine(a, b);
    int p = sk.addPoint({5.0f, 4.0f});

    Constraint d{};
    d.type = ConstraintType::DistancePointLine;
    d.entityA = p;
    d.entityB = ln;
    d.value = 4.0;
    int id = sk.addConstraint(d);

    SketchSolver solver;
    ASSERT_TRUE(solver.solve(sk, 500, 1e-4));
    ASSERT_GT(sideOf(sk, p, ln), 0.0);

    // Move it across to the mirror position: same perpendicular distance,
    // opposite side.
    sk.movePoint(p, {5.0f, -4.0f});
    solver.solve(sk, 500, 1e-4);

    EXPECT_GT(sideOf(sk, p, ln), 0.0)
        << "wrong side at the right distance was accepted as satisfied";
    EXPECT_NEAR(sk.getPoint(p)->pos.y, 4.0f, 1e-3f);
    (void)id;
}
