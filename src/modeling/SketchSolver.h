#pragma once
#include "Sketch.h"
#include "SketchConstraints.h"
#include <vector>

namespace materializr {

enum class SketchState { FullyConstrained, UnderConstrained, OverConstrained };

// Stateless-ish 2D constraint solver. Constraints live on the Sketch (so each
// sketch keeps its own set and they round-trip through ProjectIO); the solver
// only caches the most recent state/DOF for query-and-display.
//
// The legacy addConstraint / removeConstraint / getConstraints surface is
// preserved as thin wrappers around Sketch via setSketch(), so existing call
// sites don't have to change. New code can call Sketch directly.
class SketchSolver {
public:
    SketchSolver();

    // The "current" sketch for the wrapper API below. solve() also accepts a
    // sketch and updates this pointer.
    void setSketch(Sketch* s) { m_sketch = s; }

    // Wrappers that delegate to the active sketch's constraint storage.
    int addConstraint(const Constraint& c);
    void removeConstraint(int id);
    const std::vector<Constraint>& getConstraints() const;

    // Solve: adjust point positions to satisfy the sketch's constraints.
    // Returns true if converged.
    bool solve(Sketch& sketch, int maxIterations = 50, double tolerance = 1e-6);

    // Before solving a Distance whose value was just typed: slide entityB
    // along its current bearing from entityA until the pair already holds the
    // value, leaving entityA where it is. The solver alone splits the
    // correction evenly between the two points, so a free line retyped from 18
    // to 50 grew at BOTH ends and its start drifted off where it was drawn.
    // Seeding is only a starting guess: anything else constraining B or A
    // still moves them when solve() runs, exactly as before. No-op for any
    // other constraint type, a missing point, or a non-positive value.
    static void seedDistanceFromFirst(Sketch& sketch, const Constraint& c);

    // Query state from the most recent solve() call.
    SketchState getState() const;
    int degreesOfFreedom() const;

    // Drop the active sketch's constraints. Does nothing if no sketch is set.
    void clear();

private:
    Sketch* m_sketch = nullptr;
    SketchState m_state = SketchState::UnderConstrained;
    int m_dof = 0;

    // Per-constraint error and correction
    double computeError(const Constraint& c, const Sketch& sketch) const;
    void applyCorrection(const Constraint& c, Sketch& sketch, double error) const;
};

} // namespace materializr
