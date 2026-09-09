#pragma once
#include "Sketch.h"
#include <glm/glm.hpp>
#include <functional>
#include <set>
#include <vector>

// Sketch Offset tool - geometry core.
//
// Split plan/apply exactly like the Trim tool's planTrim/applyTrim: nothing
// here mutates a Sketch except applyOffset(), so the hover ghost and the
// eventual commit are computed by the same code and cannot disagree.
//
// See docs/superpowers/specs/2026-08-31-sketch-offset-tool-design.md.

namespace materializr {

// One primitive along a walked chain, in TRAVEL order. Arcs carry a SIGNED
// sweep (CCW positive) because a chain may traverse an arc backwards; the
// Sketch model itself only ever stores arcs sweeping CCW start->end.
struct OffsetSeg {
    // Spline is carried as a dense POLYLINE, not as control points. There is no
    // exact offset of a B-spline, so the honest representation of the offset is
    // the sampled curve; control points are re-fitted once, at commit, to a
    // tight tolerance (see applyOffset).
    enum class Kind { Line, Arc, Circle, Spline };

    Kind kind = Kind::Line;
    int  sourceId = -1;      // sketch element id this came from (-1 if generated)
    glm::vec2 p0{0.0f};      // Line: start. Arc: start point. Circle: point at a0.
    glm::vec2 p1{0.0f};      // Line: end.   Arc: end point.   Circle: == p0.
    glm::vec2 c{0.0f};       // Arc / Circle: centre
    float r = 0.0f;          // Arc / Circle: radius
    float a0 = 0.0f;         // Arc / Circle: start angle
    float sweep = 0.0f;      // Arc: signed sweep. Circle: +2pi.
    std::vector<glm::vec2> pts;  // Spline: the sampled curve, in travel order.
    // Spline: how many control points the source carried, so the commit-time
    // refit can start from a like-for-like density before refining.
    int srcCtrlCount = 0;

    // Point at parameter t in [0,1] along the segment, in travel order. For a
    // spline this walks the polyline by ARC LENGTH, so t is uniform in distance
    // rather than in sample index.
    glm::vec2 at(float t) const;
    // Unit tangent in travel order at parameter t.
    glm::vec2 tangent(float t) const;
    float length() const;
};

// A connected run of offsettable geometry, ordered and consistently oriented:
// segs[i].p1 == segs[i+1].p0 (and segs.back().p1 == segs.front().p0 when
// closed).
struct OffsetChain {
    std::vector<OffsetSeg> segs;
    bool closed = false;
    bool valid() const { return !segs.empty(); }
};

// Resolve the chain containing the element under `pos`.
//
// Adjacency is by shared POINT ID, which is exact - no tolerance games. The
// walk extends both ways while the shared endpoint has degree exactly 2, and
// stops at a branch, a free end, or a spline (which cannot be offset). Returns
// an empty chain when nothing was hit, or when the hit element is a spline or
// text-glyph geometry.
//
// Polygons need no special case: Sketch::addPolygon emits its edges as real
// SketchLine elements, so the walk picks them up as an ordinary closed loop.
//
// Splines ARE offsettable (an aerofoil section is two splines, and SVG imports
// carry them too): the curve is sampled, offset point-by-point along its
// normal, and re-fitted to a spline at commit.
OffsetChain walkOffsetChain(const Sketch& sk, glm::vec2 pos, float threshold);

// How an OPENING corner is closed - the two offset segments have pulled apart
// and something must bridge the gap.
enum class OffsetCorners {
    Round,  // an arc of radius |d| centred on the source vertex (always exact)
    Sharp,  // extend both segments to their intersection (miter)
};

struct OffsetResult {
    std::vector<OffsetSeg> segs;
    float distance = 0.0f;              // the signed d this was built with
    bool closed = false;
    bool valid = false;
    const char* rejectReason = nullptr; // set when !valid
};

// Offset `ch` by the SIGNED distance `d` - positive is right of travel, which
// is outward for a counter-clockwise closed chain. The caller derives the sign
// from which side of the chain the cursor is on; the chain's own travel
// direction is arbitrary (it falls out of the picked element's stored point
// order), so it must never be used to mean "outward".
//
// A Sharp miter with no intersection (near-parallel segments would meet at
// infinity) silently falls back to Round, as does any corner involving a
// spline - a sampled curve has no analytic intersection to miter to. Segments whose offset radius
// collapses to zero or inverts are dropped, leaving a gap for pruneOffset to
// resolve.
OffsetResult offsetChain(const OffsetChain& ch, float d, OffsetCorners corners);

// Distance from q to the nearest point of the chain.
float distanceToChain(const OffsetChain& ch, glm::vec2 q);

// As distanceToChain, but signed by which SIDE of the chain q lies on:
// positive is right of travel. This is how the cursor chooses the offset
// direction - for a counter-clockwise closed chain it reads as outward, but
// the walk's travel direction is arbitrary, so callers must take the sign from
// here rather than assuming one.
float signedDistanceToChain(const OffsetChain& ch, glm::vec2 q);

// Enforce the invariant that makes this tractable without a general polygon
// clipper: every point of a valid offset lies at distance exactly |d| from the
// source, and never closer. Segments that fall inside that band are dropped;
// segments that cross it are split and the valid part kept. When nothing
// survives, `valid` is cleared and `rejectReason` set.
//
// At extreme offsets this can leave a small gap where a general clipper would
// produce a clean junction. It will never emit self-intersecting geometry,
// which is the failure that actually matters.
void pruneOffset(OffsetResult& res, const OffsetChain& src, float d);

// Commit the offset into `sk` as ordinary sketch elements.
//
// `weld` returns the id of an existing sketch point coincident with the given
// position, or -1 - pass SketchTool::coincidentPoint so an offset endpoint
// landing on existing geometry joins it instead of leaving a duplicate, the
// same way Mirror does. Segments that meet each other share one point id, so
// the result is a connected chain for region building.
void applyOffset(Sketch& sk, const OffsetResult& res,
                 const std::function<int(glm::vec2)>& weld,
                 std::set<int>& outPoints, std::set<int>& outElements);

// Densified polylines for drawing. One polyline per contiguous run, so a
// pruned offset with gaps renders correctly.
void densifyChain(const OffsetChain& ch,
                  std::vector<std::vector<glm::vec2>>& out);
void densifySegs(const std::vector<OffsetSeg>& segs,
                 std::vector<std::vector<glm::vec2>>& out);

} // namespace materializr
