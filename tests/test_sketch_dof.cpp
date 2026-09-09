// Degree-of-freedom accounting for geometry whose freedoms are NOT points.
//
// The DOF counter was `2 * pointCount - numEquations`, which assumes every
// freedom in the sketch is a point coordinate. A circle's radius is stored on
// the entity itself and the solver drives it directly through
// setCircleRadius, so it is a freedom the count never granted while the
// Radius / CircleGap constraint pinning it still subtracted an equation.
//
// Net effect: every circle pushed the reported DOF one too low, so a correctly
// dimensioned circle read OverConstrained. Nothing in the app gates on solver
// state - it drives a toolbar badge - so this was a misleading readout rather
// than blocked editing.

#include "modeling/Sketch.h"
#include "modeling/SketchSolver.h"

#include <gtest/gtest.h>
#include <glm/glm.hpp>

using materializr::Constraint;
using materializr::ConstraintType;
using materializr::Sketch;
using materializr::SketchSolver;
using materializr::SketchState;

namespace {

Constraint makeRadius(int entityId, double r) {
    Constraint c{};
    c.type = ConstraintType::Radius;
    c.entityA = entityId;
    c.value = r;
    return c;
}

Constraint makeFixed(int ptId, double x, double y) {
    Constraint c{};
    c.type = ConstraintType::Fixed;
    c.entityA = ptId;
    c.value = x;
    c.valueY = y;
    return c;
}

} // namespace

// A circle has three freedoms: centre x, centre y, radius.
TEST(SketchDof, BareCircleHasThreeFreedoms) {
    Sketch sk;
    int c = sk.addPoint({0.0f, 0.0f});
    sk.addCircle(c, 5.0);

    SketchSolver solver;
    solver.solve(sk);
    EXPECT_EQ(solver.degreesOfFreedom(), 3)
        << "centre x, centre y and radius are all free";
}

// Pinning all three freedoms is exactly constrained, not over-constrained.
// This is the crisp reproduction of the reported OverConstrained symptom:
// Fixed(2) + Radius(1) = 3 equations against 3 freedoms.
TEST(SketchDof, FixedCentrePlusRadiusIsFullyConstrained) {
    Sketch sk;
    int c = sk.addPoint({0.0f, 0.0f});
    int circle = sk.addCircle(c, 5.0);
    sk.addConstraint(makeFixed(c, 0.0, 0.0));
    sk.addConstraint(makeRadius(circle, 5.0));

    SketchSolver solver;
    solver.solve(sk);
    EXPECT_EQ(solver.degreesOfFreedom(), 0);
    EXPECT_EQ(solver.getState(), SketchState::FullyConstrained)
        << "a circle with a locked centre and a driven radius is exactly "
           "determined; reporting OverConstrained puts a misleading warning "
           "on a badge for a dimension that is perfectly legal";
}

// An arc stores seven values for a shape that geometrically has five
// freedoms. The other two are the relations |start - centre| == |end - centre|
// == radius - but they may only be SUBTRACTED where something holds them. A
// driving Radius does (its correction reprojects both endpoints); with no
// driving Radius nothing does, so the arc really can reach all seven.
TEST(SketchDof, BareArcKeepsAllSevenStoredFreedoms) {
    Sketch sk;
    int c = sk.addPoint({0.0f, 0.0f});
    int s = sk.addPoint({5.0f, 0.0f});
    int e = sk.addPoint({0.0f, 5.0f});
    sk.addArc(c, s, e, 5.0);

    SketchSolver solver;
    solver.solve(sk);
    EXPECT_EQ(solver.degreesOfFreedom(), 7)
        << "with no driving Radius nothing holds |s-c| == |e-c| == radius, so "
           "all seven stored values are independently reachable";
}

// Pinning the centre and driving the radius leaves only the two bearings.
TEST(SketchDof, FixedCentrePlusRadiusOnArcLeavesTwoBearings) {
    Sketch sk;
    int c = sk.addPoint({0.0f, 0.0f});
    int s = sk.addPoint({5.0f, 0.0f});
    int e = sk.addPoint({0.0f, 5.0f});
    int arc = sk.addArc(c, s, e, 5.0);
    sk.addConstraint(makeFixed(c, 0.0, 0.0));
    sk.addConstraint(makeRadius(arc, 5.0));

    SketchSolver solver;
    solver.solve(sk);
    // 7 stored - 2 intrinsic (held by the driving Radius) - 2 Fixed - 1
    // Radius = 2: the two endpoint bearings.
    EXPECT_EQ(solver.degreesOfFreedom(), 2);
    EXPECT_NE(solver.getState(), SketchState::OverConstrained);
}

// A CircleGap pins one freedom across two circles, same as any other
// single-equation dimension - it must not read as over-constrained.
TEST(SketchDof, CircleGapDoesNotOverConstrain) {
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
    sk.addConstraint(gap);

    SketchSolver solver;
    solver.solve(sk);
    // 2 circles = 6 freedoms, one gap equation.
    EXPECT_EQ(solver.degreesOfFreedom(), 5);
    EXPECT_NE(solver.getState(), SketchState::OverConstrained);
}

// Sketches with no radius-bearing geometry must be completely unaffected by
// the accounting change - this is the regression guard on the old behaviour.
TEST(SketchDof, PointOnlySketchUnchanged) {
    Sketch sk;
    int a = sk.addPoint({0.0f, 0.0f});
    int b = sk.addPoint({10.0f, 0.0f});
    sk.addLine(a, b);

    Constraint d{};
    d.type = ConstraintType::Distance;
    d.entityA = a;
    d.entityB = b;
    d.value = 10.0;
    sk.addConstraint(d);

    SketchSolver solver;
    solver.solve(sk);
    EXPECT_EQ(solver.degreesOfFreedom(), 3); // 4 coords - 1 equation
}
