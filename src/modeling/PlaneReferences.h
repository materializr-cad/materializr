#pragma once
#include "Sketch.h"
#include <TopoDS_Shape.hxx>
#include <vector>

// Snap references for a sketch on a plane: the geometry that already lies ON
// that plane, so the cursor can land on a body's outline or on another
// sketch's points without anything being drawn first.
//
// Both functions MERGE into the sketch's existing FaceReference (what the
// "sketch on a face" path already pulled from the host face), de-duplicating
// against it. They run once, on sketch entry - never per mouse move.

namespace materializr {

struct PlaneReferenceStats {
    int points = 0, lines = 0, circles = 0;
    bool crossSection = false;   // a body was cut by the plane and its section added
    bool skippedHeavy = false;   // a body was too large to scan and was left out
};

// Edges and vertices of `bodies` lying on the sketch plane, plus - where the
// plane cuts through a body - the cross-section outline on it (only for
// bodies of modest size: it is a boolean section, far too heavy for a big
// part, which is skipped and reported through skippedHeavy).
PlaneReferenceStats addBodyPlaneReferences(Sketch& sk,
                                           const std::vector<TopoDS_Shape>& bodies,
                                           bool crossSections = true);

// Points, lines and circles of other sketches that lie on this sketch's plane
// (same plane, either facing). Sketches on other planes are ignored.
PlaneReferenceStats addSketchPlaneReferences(Sketch& sk,
                                             const std::vector<const Sketch*>& others);

} // namespace materializr
