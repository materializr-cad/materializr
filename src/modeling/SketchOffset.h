#pragma once
#include "Sketch.h"
#include <glm/glm.hpp>
#include <set>
#include <vector>

// Offset a closed sketch loop by a signed distance, producing EDITABLE sketch
// entities (lines and arcs) rather than a sampled polyline.
//
// Pure geometry: reads a Sketch, writes nothing, needs no GL and no SketchTool.
// Testable headless — same shape as SectionCap. The interactive tool in
// SketchTool drives this; the split exists so every refusal path below can be
// tested without a window.
//
// Contract: docs/specs/spec-sketch-offset/. The refusal cases are not
// defensive padding — each was measured against OCCT 7.9.3, and several
// SUCCEED at the OCCT level while producing meaningless geometry. See
// occt-offset-findings.md.

namespace materializr {

// Why an offset was refused. Each maps to its own user-facing message, because
// "offset failed" is useless when the fix differs per cause.
enum class OffsetError {
    None = 0,
    EmptySelection,     // nothing selected
    UnsupportedEntity,  // spline in the selection
    OpenChain,          // a vertex of degree 1 — not a closed loop
    Branching,          // a vertex of degree > 2
    Disconnected,       // more than one connected component
    DegenerateEntity,   // zero-length entity, or two entities on the same span
    SelfIntersecting,   // graph-closed but crossing itself (a bow-tie)
    DistanceTooSmall,   // |d| below the floor, or non-finite
    OffsetFailed,       // OCCT reported IsDone() == false
    OffsetCollapsed,    // IsDone() but the shape came back empty
    OffsetSplit,        // the result split into more than one wire
    UnsupportedResult,  // result not closed/valid, or an unrepresentable curve
};

// Message for the toast. Present tense, names the cause, no error codes.
const char* offsetErrorMessage(OffsetError e);

// The entities a loop is made of. Ids into the Sketch.
struct OffsetSource {
    std::set<int> lineIds;
    std::set<int> arcIds;
    std::set<int> circleIds;   // a lone circle is a valid loop on its own

    bool empty() const { return lineIds.empty() && arcIds.empty() && circleIds.empty(); }
};

// What an offset WOULD create. Built in full before the Sketch is touched, so
// a failure midway leaves nothing behind: `recordSketchMutation` snapshots for
// undo but does not roll back a partial write, and Sketch's mutators append
// directly. Phase 1 = build this, phase 2 = apply it.
struct OffsetPlan {
    struct Line   { glm::vec2 a, b; };
    struct Arc    { glm::vec2 center, start, end; double radius; };
    struct Circle { glm::vec2 center; double radius; };

    std::vector<Line>   lines;
    std::vector<Arc>    arcs;
    std::vector<Circle> circles;

    bool empty() const { return lines.empty() && arcs.empty() && circles.empty(); }
    size_t size() const { return lines.size() + arcs.size() + circles.size(); }
};

// --- Tolerances -------------------------------------------------------------
// Scale-aware, because SketchPoint::pos is glm::vec2 (single precision) and a
// float ULP is 7.6e-6 mm at 100 mm and 6.1e-5 mm at 1000 mm. A fixed 1e-6
// predicate falls below representable resolution over most of the working
// range and changes meaning as a sketch is translated away from the origin.

// Coincidence tolerance at a point's own magnitude.
double weldTol(glm::vec2 p);

// Smallest offset that can be asked for on this loop. Tied to weldTol so an
// offset can never be smaller than the distance at which its own output would
// weld back onto itself.
double minOffsetDistance(const Sketch& sk, const OffsetSource& src);

// --- Operations -------------------------------------------------------------

// Gather the loop implied by a selection. Splines are reported rather than
// skipped: silently ignoring one would offset a different shape than the user
// selected.
OffsetError gatherSource(const Sketch& sk,
                         const std::set<int>& selLines,
                         const std::set<int>& selArcs,
                         const std::set<int>& selCircles,
                         const std::set<int>& selSplines,
                         OffsetSource& out);

// Validate the loop: one closed, simple, non-degenerate component.
// Adjacency is two-stage — exact shared point id first (the only stage drawn
// geometry ever needs), then a weldTol union of still-distinct coincident
// endpoints, without which an imported loop reads as an open chain.
OffsetError validateSource(const Sketch& sk, const OffsetSource& src);

// Signed distance from `p` to the loop: negative inside, positive outside.
// Used to turn a cursor position into a direction. Ties resolve outward.
double signedDistanceToLoop(const Sketch& sk, const OffsetSource& src, glm::vec2 p);

// Validate, offset by `distance` (positive = outward), and convert the result
// into a plan. Returns OffsetError::None and fills `out` on success; on any
// failure `out` is left empty. Never throws: OCCT failures become errors.
OffsetError computeOffsetPlan(const Sketch& sk, const OffsetSource& src,
                              double distance, OffsetPlan& out);

// Apply a plan to a sketch. Pure bookkeeping — no geometry that can fail, so
// this is the only phase that mutates. Welds result-to-result endpoints at
// weldTol; never welds onto source geometry (findCoincidentPoint's
// 0.3*snapScale UI radius would destroy any offset smaller than a snap).
void applyOffsetPlan(Sketch& sk, const OffsetPlan& plan,
                     std::set<int>* outPoints = nullptr,
                     std::set<int>* outEntities = nullptr);

} // namespace materializr
