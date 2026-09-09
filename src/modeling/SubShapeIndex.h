#pragma once
#include <TopoDS_Shape.hxx>
#include <TopoDS_Edge.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <string>
#include <vector>

// Persistent sub-shape identity for the save framework (Tier 2b).
//
// OCCT TopoDS handles (the edges a fillet blends, the faces a shell opens)
// don't survive save/load - but BREP write/read round-trips a shape's
// topology byte-identically, so a sub-shape's 1-based position in
// TopExp::MapShapes(shape, type) is stable across sessions FOR THE SAME
// SHAPE. Ops therefore serialise their sub-shape references as indices into
// their step's INPUT shape and re-resolve them on reload against the very
// same shape restored from the file.
//
// The indices are only meaningful against that exact shape: if an upstream
// edit regenerates the input body, resolution fails (or the downstream
// recompute fails) - the documented downstream-edit limitation.
namespace SubShapeIndex {

// 1-based index of `sub` within `shape`'s canonical map of `type` sub-shapes;
// 0 if `sub` isn't part of `shape`.
int indexOf(const TopoDS_Shape& shape, const TopoDS_Shape& sub,
            TopAbs_ShapeEnum type);

// Resolve a 1-based index back to the sub-shape; null shape if out of range.
TopoDS_Shape at(const TopoDS_Shape& shape, int index, TopAbs_ShapeEnum type);

// Comma-joined indices for a list of sub-shapes ("3,17,42"). Sub-shapes that
// don't belong to `shape` are skipped.
std::string serialize(const TopoDS_Shape& shape,
                      const std::vector<TopoDS_Shape>& subs,
                      TopAbs_ShapeEnum type);

// Parse "3,17,42" back into indices. Returns empty on malformed input.
std::vector<int> parse(const std::string& csv);

// Resolve a parsed index list against `shape`. Returns false (and clears
// `out`) if ANY index fails to resolve - partial resolution would silently
// operate on the wrong geometry.
bool resolveAll(const TopoDS_Shape& shape, const std::vector<int>& indices,
                TopAbs_ShapeEnum type, std::vector<TopoDS_Shape>& out);

// Geometric re-binding: when an upstream edit regenerates a body, stored
// TopoDS_Edge handles no longer exist in the new shape (fresh TShapes), even
// though the model edge is conceptually still there - possibly trimmed
// differently (e.g. a box edge shortened by a re-filleted neighbour). For
// each edge not present in `shape`, find its successor by underlying carrier
// geometry: same line (direction + point-on-line) or same circle (centre,
// axis, radius), tie-broken by nearest midpoint. Edges already in `shape`
// pass through unchanged. Returns false (leaving `edges` untouched) if ANY
// edge can't be re-bound - e.g. the edge was genuinely consumed.
bool rebindEdges(const TopoDS_Shape& shape, std::vector<TopoDS_Edge>& edges);

} // namespace SubShapeIndex
