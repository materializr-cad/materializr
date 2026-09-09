#pragma once
#include <TopoDS_Shape.hxx>
#include <TopoDS_Edge.hxx>
#include <utility>
#include <vector>
#include <string>

namespace materializr { class Sketch; }

// Generative edge tracking (experiment/generative-edges).
//
// A fillet/chamfer stores WHICH SKETCH FEATURE produced each edge it operates
// on, instead of an absolute position or an ordinal index. After a sketch
// DIMENSION edit relocates the edge, it is re-found from the sketch element's
// CURRENT position - surviving resizes that break ordinal/carrier matching.
//
// Real bodies are carved by SEVERAL sketches (base extrude + profile cuts), so
// every anchor carries its own sketch id and each edge is classified against
// every sketch in the document. Edge kinds:
//   Corner - straight edge parallel to a sketch's normal, sitting over a
//            single sketch VERTEX (point id).
//   Rim    - straight edge at a fixed height along a sketch's normal, lying ON
//            a sketch LINE's segment. Intersecting features clip rims into
//            fragments, so this is an on-segment test (not endpoint-pair
//            equality); `h` plus the fractional midpoint `t` disambiguate
//            between fragments and between top/bottom caps.
//   Arc    - circular/elliptical edge cut from the cylinder swept by a sketch
//            ARC: its center lies on the arc's extrusion axis and its radius
//            (minor radius for oblique = elliptical sections) equals the arc
//            radius. `h` = center height along the normal.
//   Circle - same test for a full sketch CIRCLE (hole rims).
namespace EdgeAnchor {

struct Anchor {
    enum Kind { None, Corner, Rim, Arc, Circle } kind = None;
    int    sketchId = -1;
    int    elemId   = -1;  // Corner: point id; Rim: line id; Arc/Circle: arc/circle id
    double h        = 0.0; // height along the sketch normal (see kinds above)
    double t        = 0.5; // Rim only: midpoint fraction along the sketch line
};

// (document sketch id, sketch) pairs - the caller supplies every sketch it has.
using SketchRef = std::pair<int, const materializr::Sketch*>;

// One anchor per input edge (Kind::None for edges no sketch can attribute).
std::vector<Anchor> compute(const std::vector<TopoDS_Edge>& edges,
                            const std::vector<SketchRef>& sketches);

// Re-find each anchored edge in `base` at its sketch element's CURRENT
// position. Candidates matching the element are disambiguated by nearest
// (h, t); each anchor takes a distinct edge. Returns false (out cleared)
// unless EVERY anchor is non-None and resolves - a partial result would
// fillet the wrong geometry.
bool resolve(const std::vector<Anchor>& anchors,
             const std::vector<SketchRef>& sketches,
             const TopoDS_Shape& base, std::vector<TopoDS_Edge>& out);

// Serialize as "v2~<a0>~<a1>..." where each edge token is
// "C,<sid>,<pid>,<h>" | "R,<sid>,<lid>,<h>,<t>" | "A,<sid>,<aid>,<h>" |
// "O,<sid>,<cid>,<h>" | "N". Empty if there is nothing useful to store.
// parse() also accepts the legacy v1 form "<sketchId>~C,<pid>~R,<lid>,<h>".
std::string serialize(const std::vector<Anchor>& anchors);
bool parse(const std::string& blob, std::vector<Anchor>& anchors);

} // namespace EdgeAnchor
