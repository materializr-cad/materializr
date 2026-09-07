#pragma once
#include <glm/glm.hpp>
#include <vector>

namespace materializr {

enum class ConstraintType {
    Coincident,    // two points same position
    Horizontal,    // line is horizontal
    Vertical,      // line is vertical
    Distance,      // fixed distance between two points
    Radius,        // fixed radius of circle/arc
    Parallel,      // two lines are parallel
    Perpendicular, // two lines are perpendicular
    Fixed,         // point locked in place
    Tangent,       // arc/circle tangent to line
    Equal,         // two lines have equal length
    Concentric,    // two circles/arcs share same center
    Angle,         // fixed angle (radians) between two lines
    DistancePointLine, // fixed perpendicular distance from a point to a line's infinite carrier
    CircleGap          // fixed rim-to-rim gap between two circles/arcs (centre dist - rA - rB)
};

struct Constraint {
    int id;
    ConstraintType type;
    int entityA = -1;  // point or line id
    int entityB = -1;  // second entity (for Coincident, Parallel, Perpendicular)
    double value = 0.0;  // Distance / Radius / Angle. For Fixed, the X coord.
    double valueY = 0.0; // Y coord of the locked position (Fixed only).
    bool isSatisfied = false;
    // Sketch-space offset of the dimension label from its auto-computed
    // anchor. (0,0) = legacy auto placement (pre-offset files and constraints
    // created without explicit label placement).
    double labelOffX = 0.0;
    double labelOffY = 0.0;

    // Driving (default) = the solver enforces this constraint and it consumes
    // a degree of freedom. Reference/driven = the solver ignores it entirely;
    // it only annotates, re-measuring itself as the geometry moves.
    //
    // Defaults to TRUE so every pre-existing constraint — in memory, in old
    // project files, and every geometric type (Horizontal, Coincident,
    // Tangent, …) for which "reference" is meaningless — keeps driving
    // exactly as before. Only the Dimension tool clears it, so a placed
    // dimension measures without moving anything until the user promotes it
    // in the label's edit popup. See Application::applyPendingDimension.
    bool isDriving = true;

    // Which way round the constraint was placed. Both error terms below are
    // unsigned — a perpendicular distance and a point-to-point length are the
    // same number on either side — so a dimension driven through zero came out
    // the far side and the sketch settled mirrored. These record the
    // arrangement the user placed, so a correction can restore it instead of
    // re-deriving it from geometry that has already crossed over.
    //
    //   DistancePointLine: orientX is the side, +1 or -1, as the sign of
    //     cross(lineDir, point - lineStart). Relative to the line's own
    //     direction, so it survives the line rotating.
    //   Distance: (orientX, orientY) is the unit A->B direction, used only to
    //     break the tie when the two points are coincident and the geometry
    //     itself offers no direction.
    //
    // (0, 0) means "not recorded" — projects written before this existed, and
    // constraints whose geometry was degenerate when they were seeded. Those
    // keep the previous behaviour. Seeded on first solve, not at creation, so
    // the dimension-placement paths did not have to change.
    double orientX = 0.0;
    double orientY = 0.0;
};

// Reference/driven mode is only meaningful for the dimension-bearing types —
// the ones carrying a numeric value the user reads off the drawing. A
// geometric relationship (Horizontal, Parallel, Coincident, …) has no
// measurement to annotate, so it is always driving and the edit popup offers
// no toggle for it.
inline bool constraintSupportsReference(ConstraintType t) {
    return t == ConstraintType::Distance || t == ConstraintType::Radius ||
           t == ConstraintType::Angle || t == ConstraintType::DistancePointLine ||
           t == ConstraintType::CircleGap;
}

// How far a press must travel, in pixels, before it counts as dragging a
// dimension label rather than clicking it. Pixels rather than sketch mm because
// this is a property of the hand, not of the model's scale.
inline constexpr float kDimDragThresholdPx = 3.0f;

// Whether a press that has moved (dx, dy) pixels is a drag yet.
//
// Load-bearing for more than feel: the caller must store NOTHING until this
// turns true. A label's stored offset doubles as the "user placed this" flag,
// so writing one on a plain click converts an automatically positioned label
// into a fixed one — it gains a leader line and stops tracking automatic
// placement, without the user having asked for either.
inline bool dimDragExceedsThreshold(float dx, float dy) {
    return dx * dx + dy * dy > kDimDragThresholdPx * kDimDragThresholdPx;
}

// Label offset for a dimension tag dropped at `want`, measured from its
// geometric anchor.
//
// (0,0) is overloaded: it is also the sentinel for "never placed", which the
// renderer reads as "use the automatic position". A tag dropped exactly on its
// anchor would therefore snap back to auto placement and look like the drag was
// ignored. Nudging by a tenth of a micron is invisible at any usable zoom and
// keeps the placement.
inline void dimLabelOffset(double wantX, double wantY,
                           double anchorX, double anchorY,
                           double& outX, double& outY) {
    outX = wantX - anchorX;
    outY = wantY - anchorY;
    if (outX == 0.0 && outY == 0.0) outX = 1e-4;
}

} // namespace materializr
