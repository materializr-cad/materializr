#pragma once

#include <Bnd_Box.hxx>
#include <Poly_Triangulation.hxx>
#include <Standard_Handle.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Pln.hxx>
#include <gp_Trsf.hxx>
#include <glm/glm.hpp>

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
    // Poly_Triangulation's node winding follows the face's underlying
    // Geom_Surface parametrization, NOT the face's true outward normal - a
    // TopAbs_REVERSED face (about half of any shape's faces, typically) has
    // its winding backwards relative to the solid. sliceSection/
    // computeSectionCap never cared (they only measure vertex DISTANCE to a
    // plane), but computeMeshSilhouette computes actual triangle normals to
    // classify front/back-facing, and got this wrong before `reversed`
    // existed - see the "OCCT outward-normal trap" this codebase has hit
    // before. true = flip the raw cross-product normal to get the real one.
    bool reversed = false;
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
    // Same closed loops `lines`/`cap` are drawn from, but ORDERED and in the
    // cutting plane's own 2D (X,Y) coordinates (its XDirection/YDirection) -
    // exactly Sketch::addPoint's coordinate convention, so a caller can turn
    // a loop straight into real sketch points/lines (see MeshTracePlugin's
    // "Insert Outline into Sketch"). Every closed loop is included (both
    // outer profile and hole loops); open chains (an unmeshed face) are NOT,
    // since there's no sensible way to close them into a sketch region.
    std::vector<std::vector<glm::vec2>> loops;
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

// "Shadow" capture (mesh-trace's Shadow mode, see Document.h's
// MeshTraceMode): the object's overall SILHOUETTE BLOB as seen along the
// plane's normal, regardless of where the plane sits relative to the body
// (unlike sliceSection, which only sees what the plane's CURRENT position
// actually touches) - like a real drop shadow, no internal detail (a rib, a
// boss, a visible hole rim), just the outer profile.
//
// Two functions, two different jobs - an earlier version tried to make ONE
// algorithm (per-triangle front/back-facing classification vs. the view
// direction, silhouette = where adjacent triangles disagree) do both, and
// it was the wrong call: classification is fast and was visually correct
// on every axis actually looked at, but numerically fragile on a dense/
// curved real-world mesh viewed along an axis with many near-tangent
// triangles (classification noise fragmented the loop instead of chaining
// cleanly) - exactly the failure the Right-axis fix (an earlier occlusion
// filter via BRepClass3d_SolidClassifier) did NOT cover, because it was a
// different bug in the same fragile technique. Splitting the two jobs
// means the simple, robust one keeps doing the job it's already proven at
// (every frame, live), and the expensive, slower one only runs once, on
// click, where its cost doesn't matter:
//
//  computeMeshShadow: every triangle flattened onto the plane, no
//    classification, no loop-chaining at all - just raw filled triangles
//    appended to `outPositions` (x,y,z per vertex, same convention as
//    computeSectionCap). Cheap enough for the live overlay every frame,
//    and immune to both failure modes above since it never tries to find
//    an edge at all - overlapping/occluded flattened triangles simply
//    z-fight visually, invisible to the viewer looking straight along the
//    normal, which is the only way Shadow mode is ever viewed.
//
//  computeMeshShadowOutline: the same flattened triangles, but rasterized
//    onto a bounded-resolution occupancy grid (a triangle covers a cell iff
//    its centre is inside the triangle) and traced into ordered loops via
//    the shared finishSlice - the boundary of every occupied cell facing an
//    unoccupied neighbour is one segment, exactly the "voxel face culling"
//    used for 2D/3D voxel mesh outlines. A first version instead tried an
//    exact 2D union of real BRep faces (BOPAlgo_Builder General Fuse +
//    ShapeUpgrade_UnifySameDomain); on a real ~1300-face/5400-triangle mesh
//    that took over an hour and still produced 1427 fragmented loops with
//    an internal OCCT triangulation error - exact arithmetic over that many
//    nearly-coplanar-by-construction pieces doesn't scale and doesn't stay
//    numerically sound. Rasterizing has neither problem: marking a cell is
//    a plain boolean OR, so it doesn't care which triangle is "in front" or
//    how many there are, and it's proportional to triangle count + grid
//    cells rather than pairwise triangle-triangle intersections - the same
//    real mesh traces in a few hundred milliseconds for all three planes.
//
// Same SectionSlice shape as sliceSection for computeMeshShadowOutline:
// `lines` (outline), `cap` (filled), `loops` (ordered, ready for Sketch
// insertion via Application::insertMeshTraceIntoSketch).
bool computeMeshShadow(const TopoDS_Shape& shape, const gp_Pln& plane,
                       std::vector<float>& outPositions);
bool computeMeshShadowOutline(const TopoDS_Shape& shape, const gp_Pln& plane,
                              SectionSlice& out);

} // namespace materializr
