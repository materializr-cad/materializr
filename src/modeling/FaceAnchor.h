#pragma once
#include <TopoDS_Shape.hxx>
#include <TopoDS_Face.hxx>
#include <utility>
#include <vector>
#include <string>

namespace materializr { class Sketch; }

// Generative FACE tracking (experiment/face-anchors) - the dual of EdgeAnchor.
//
// A face on a sketch-extrude body is named by WHICH SKETCH FEATURE generated
// it, not by an ordinal index or absolute position, so it survives a sketch
// dimension edit that relocates it. This is the primitive the deferred
// topological-naming work needs to make face push/pull, shell open-faces and
// thread targets re-resolvable across history rebuilds (see the topo-naming
// notes). Face kinds:
//   Wall - a planar side face whose normal is PERPENDICULAR to the sketch
//          normal: the wall swept from a sketch LINE. Named by (sketch, line);
//          `cu,cv` is its centroid in sketch coords (fragment disambiguation).
//   Cyl  - a cylindrical face swept from a sketch ARC or CIRCLE: its axis is
//          parallel to the sketch normal, over the arc/circle centre, radius =
//          the sketch radius. Named by (sketch, arc/circle). This is the one
//          a THREAD targets.
//   CurveWall - a surface-of-extrusion side face swept from a sketch SPLINE
//          (a non-circular curve): the extrusion direction is parallel to the
//          sketch normal and its profile lies on the spline. Named by
//          (sketch, spline). Matched by sampling the face's basis curve and
//          testing it lies on the spline's sampled curve.
//   Cap  - a planar face whose normal is PARALLEL to the sketch normal: the
//          top/bottom cap of the extrude. Not tied to a single sketch element
//          (it's the whole region), so it is the weakest to resolve under a
//          move - kept for coverage, resolved by nearest (h, centroid).
namespace FaceAnchor {

struct Anchor {
    enum Kind { None, Wall, Cyl, CurveWall, Cap } kind = None;
    int    sketchId = -1;
    int    elemId   = -1;  // Wall: line id; Cyl: arc/circle id; CurveWall: spline id; Cap: -1
    double h        = 0.0; // centroid height along the sketch normal
    double cu       = 0.0; // centroid u in sketch coords
    double cv       = 0.0; // centroid v in sketch coords
};

// (document sketch id, sketch) pairs - the caller supplies every sketch it has.
using SketchRef = std::pair<int, const materializr::Sketch*>;

// One anchor per input face (Kind::None for faces no sketch can attribute -
// blends, imported/primitive faces, faces from non-sketch features).
std::vector<Anchor> compute(const std::vector<TopoDS_Face>& faces,
                            const std::vector<SketchRef>& sketches);

// Re-find each anchored face in `base` at its sketch element's CURRENT
// position. Candidates matching the element are disambiguated by nearest
// (h, centroid); each anchor takes a distinct face. Returns false (out
// cleared) unless EVERY anchor is non-None and resolves.
bool resolve(const std::vector<Anchor>& anchors,
             const std::vector<SketchRef>& sketches,
             const TopoDS_Shape& base, std::vector<TopoDS_Face>& out);

// Serialize as "v1~<a0>~<a1>..." where each face token is
// "W,<sid>,<lid>,<h>,<cu>,<cv>" (wall) | "Y,<sid>,<cid>,<h>,<cu>,<cv>" (cyl) |
// "E,<sid>,<spid>,<h>,<cu>,<cv>" (curve wall) | "P,<sid>,<h>,<cu>,<cv>" (cap) |
// "N". Empty if nothing useful to store.
std::string serialize(const std::vector<Anchor>& anchors);
bool parse(const std::string& blob, std::vector<Anchor>& anchors);

} // namespace FaceAnchor
