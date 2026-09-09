#pragma once
// Cut-based blend fallback (#55): when OCCT's native blend (ChFi3d via
// BRepFilletAPI_MakeChamfer) fails because a surface feature crosses the
// target edge - a drilled hole or pocket fragments the edge and the blend
// surface can't resolve against the feature walls - build the SAME material
// removal as a boolean instead: sweep the chamfer's triangular cross-section
// along the edge line and subtract it. Booleans are the robust half of the
// kernel; a wedge swept ACROSS the feature gap gives exactly the geometry
// you'd get by reordering history so the chamfer preceded the feature.
//
// Strictly a fallback: callers reach this only after the native build failed,
// so models where OCCT succeeds never touch this code. Scope (anything
// outside it returns false and the op fails exactly as before):
//   - straight edges between two PLANAR faces, CONVEX only (a cut can only
//     remove material; a concave chamfer adds it)
//   - collinear selected edges sharing the same plane pair merge into ONE
//     sweep span, so selecting the fragments on both sides of a hole cuts
//     the bevel straight through it.

#include "GenerationLedger.h"
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <vector>

namespace materializr {
namespace blendcut {

// Chamfer as a swept-wedge cut. dRef is the setback measured along `refFace`
// when non-null (the asymmetric reference), else along each edge's first
// adjacent face - mirroring BRepFilletAPI_MakeChamfer::Add(dRef, dOther,
// edge, face). On success fills `outShape` (the cut body, BRepCheck-valid,
// strictly smaller in volume), `outBlendFaces` (the bevel faces ON the
// result, for click-to-edit / highlighting), and captures the cut's
// generation maps into `ledger` so face lineage propagates as usual.
bool cutChamfer(const TopoDS_Shape& body,
                const std::vector<TopoDS_Edge>& edges,
                double dRef, double dOther,
                const TopoDS_Face& refFace,
                topo::GenerationLedger& ledger,
                TopoDS_Shape& outShape,
                std::vector<TopoDS_Shape>& outBlendFaces);

// Concave (interior-corner) chamfer as a FILL: an inside corner is chamfered
// by ADDING a ramp, which native OCCT refuses when the ramp's footprint
// crosses a feature (a hole in the floor face). Fuse a ramp prism swept over
// the full span, then re-pierce each crossed void with its own outline so a
// hole stays open - exactly chamfer-first-then-feature. Same gating: only
// called after native failed; straight edges between planar faces; refuses
// convex edges (those belong to cutChamfer).
bool fillChamfer(const TopoDS_Shape& body,
                 const std::vector<TopoDS_Edge>& edges,
                 double dRef, double dOther,
                 const TopoDS_Face& refFace,
                 topo::GenerationLedger& ledger,
                 TopoDS_Shape& outShape,
                 std::vector<TopoDS_Shape>& outBlendFaces);

// Convex fillet as a cut: the same swept wedge, but bounded by the arc of
// radius `radius` tangent to both adjacent faces - subtracting it leaves
// exactly the fillet cylinder. Same scope and gating as cutChamfer.
bool cutFillet(const TopoDS_Shape& body,
               const std::vector<TopoDS_Edge>& edges, double radius,
               topo::GenerationLedger& ledger,
               TopoDS_Shape& outShape,
               std::vector<TopoDS_Shape>& outBlendFaces);

} // namespace blendcut
} // namespace materializr
