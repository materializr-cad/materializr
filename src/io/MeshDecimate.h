#pragma once
#include <array>
#include <vector>

#include <gp_Pnt.hxx>

namespace materializr {

// A bare indexed triangle mesh: shared vertices + integer triangle indices.
struct SimpleMesh {
    std::vector<gp_Pnt> nodes;             // 0-based vertices
    std::vector<std::array<int, 3>> tris;  // indices into nodes
};

// Reduce `mesh` toward `targetTriangles` using Garland–Heckbert quadric
// error-metric edge collapse. Manifold-preserving (collapses that would flip a
// triangle are rejected) and boundary-preserving (open edges are pinned). A
// no-op when the mesh already has <= targetTriangles triangles. Returns the
// resulting triangle count.
//
// QEM naturally collapses flat regions first (their quadric error is ~0) while
// preserving edges/corners - so it both bounds the cost of the downstream B-rep
// build (sewing N facets is the import bottleneck) and pre-merges fairly-flat
// regions, which is exactly what makes them sketchable after import.
int decimateMesh(SimpleMesh& mesh, int targetTriangles);

} // namespace materializr
