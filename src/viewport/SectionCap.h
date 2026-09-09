#pragma once

#include <Bnd_Box.hxx>
#include <Poly_Triangulation.hxx>
#include <Standard_Handle.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Pln.hxx>
#include <gp_Trsf.hxx>

#include <vector>

namespace materializr {

// One face's triangulation as placed in the world. A snapshot of these is
// what the section worker reads. Handle refcounts are atomic and nothing in
// the app edits a Poly_Triangulation in place (a re-mesh replaces the face's
// handle), so the main thread can hand these to a worker without copying the
// body and re-mesh underneath it safely.
struct FaceMesh {
    Handle(Poly_Triangulation) tri; // null for a face the mesher left bare
    Bnd_Box box;                    // bare faces only: where the gap in the slice would be
    gp_Trsf trsf;
    bool moved = false;
};
std::vector<FaceMesh> faceMeshes(const TopoDS_Shape& shape);

// The cross-section of a body: every place the plane passes through its mesh.
// `lines` is the section outline as segments (x,y,z,x,y,z per segment): every
// edge of each closed loop, plus open chains where a face had no
// triangulation. `cap` is the filled section as triangles (x,y,z per vertex),
// the region where the plane passes through solid material, so a clipped
// solid does not read as a hollow shell; only closed loops can be filled.
// Keeps the -normal half-space, matching the viewport shader which discards
// the +normal side.
struct SectionSlice {
    std::vector<float> lines;
    std::vector<float> cap;
};

// Slice the meshes with the plane. Each triangle the plane crosses yields a
// segment, the segments are joined into loops, and the loops are filled.
// Returns true if any line or cap was produced. Pure OCCT (no GL), so it is
// testable headless.
bool sliceSection(const std::vector<FaceMesh>& faces, const gp_Pln& cuttingPlane,
                  SectionSlice& out);

// Cap only, from the shape's own triangulation. Appends triangle positions to
// `outPositions`; returns true if any cap was produced.
bool computeSectionCap(const TopoDS_Shape& shape, const gp_Pln& cuttingPlane,
                       std::vector<float>& outPositions);

} // namespace materializr
