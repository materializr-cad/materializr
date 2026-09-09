#pragma once

#include <TopoDS_Face.hxx>
#include <gp_Vec.hxx>

#include <vector>

namespace materializr {

// Triangles for the swept tool volume of `profile` along `sweep`, built from
// the profile's OWN triangulation and never from the mesher: both caps are
// the profile's triangles (one of them translated), the walls are quads
// between consecutive nodes of each boundary edge's polygon on that
// triangulation. Six floats per vertex (position, normal), appended to `out`.
//
// The mesher on a swept prism paid per side face, not per cap: a profile
// with 300 holes sweeps to 304 walls and 26 ms of BRepMesh per drag frame;
// this is microseconds. Returns false, appending nothing, when the profile
// has no triangulation or an edge has no polygon on it; the caller then
// falls back to meshing the prism.
bool ghostPrismMesh(const TopoDS_Face& profile, const gp_Vec& sweep,
                    std::vector<float>& out);

} // namespace materializr
