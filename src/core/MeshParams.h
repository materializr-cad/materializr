#pragma once
// How every mesher call in Materializr asks OCCT to triangulate a shape.
//
// The one setting that matters is the algorithm. OCCT's default (Watson)
// slows down sharply on a planar face with many curved inner wires, which is
// exactly what hole patterns and SVG outlines produce: one face of a 400-hole
// plate took 1028 ms, the whole body 1095 ms, on every edit and every load.
// Delabella (in OCCT since 7.6; every platform here builds 7.9.3) meshes that
// face in 47 ms and the body in 84 ms, was never slower on spheres, tori,
// fillets or slotted plates, and gives the same triangle counts with equal or
// smaller achieved deflection.
//
// BRepMesh_IncrementalMesh's shape constructors already call Perform(); a
// second Perform() is a full extra pass over the model (25 ms on that plate),
// so callers must not add one.
#include <IMeshTools_Parameters.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Tool.hxx>
#include <Poly_Triangulation.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <algorithm>

namespace materializr {

inline IMeshTools_Parameters meshParams(double deflection, double angularDeflection,
                                        bool inParallel)
{
    IMeshTools_Parameters p;
    p.Deflection = deflection;
    p.Angle = angularDeflection;
    p.Relative = false;
    p.InParallel = inParallel;
    p.MeshAlgo = IMeshTools_MeshAlgoType_Delabella;
    return p;
}

// A face where even the Watson retry above fails - real and reproducible on
// an entirely ordinary planar face (10-edge boundary, no degenerate edges),
// not just a theoretical edge case: confirmed mesh-failing under BOTH
// Delabella and Watson at a project's "High"/"Ultra" quality settings
// (0.03/0.15 and 0.01/0.10) while succeeding fine at "Medium" (0.10/0.30)
// and coarser. Whatever OCCT's samplers dislike about that fine a spacing on
// this face, backing off clears it - recovered at 2x deflection for the
// High-quality case above, 4x for Ultra. A locally coarser single face is a
// far smaller defect than a hole clean through the model, so escalate
// through a few factors (capping angular deflection around 34 degrees,
// where facets stop looking meaningfully different) before giving up.
inline bool meshBareFaceEscalating(const TopoDS_Face& f, double deflection,
                                   double angularDeflection) {
    TopLoc_Location loc;
    for (double factor : {2.0, 4.0, 8.0, 16.0, 32.0}) {
        try {
            IMeshTools_Parameters wp = meshParams(
                deflection * factor, std::min(angularDeflection * factor, 0.6), false);
            wp.MeshAlgo = IMeshTools_MeshAlgoType_Watson;
            BRepMesh_IncrementalMesh(f, wp);
        } catch (...) {
            continue;
        }
        if (!BRep_Tool::Triangulation(f, loc).IsNull()) return true;
    }
    return false;
}

// Delabella (meshParams()'s algorithm, chosen for speed on many-holed faces)
// can leave a face with zero triangles on otherwise fully BRepCheck-valid,
// closed geometry - confirmed on real boolean-result bodies (issue #117).
// Mesh with it, then retry any bare face with Watson (OCCT's default, slower
// but far more robust) so a real hole never reaches the screen. Every caller
// that meshes a shape for DISPLAY (as opposed to STL/OBJ/glTF export, which
// tolerate a slower, non-Delabella pass across the board) should go through
// this instead of constructing BRepMesh_IncrementalMesh directly - a plain
// Delabella call has already had to be patched into this fallback more than
// once (the main render path, and again for the async worker a live
// interactive op's result gets pre-meshed on) precisely because it is easy to
// add a new meshing call site without remembering the fallback.
inline void meshWithFallback(const TopoDS_Shape& shape, double deflection,
                             double angularDeflection, bool inParallel)
{
    BRepMesh_IncrementalMesh(shape, meshParams(deflection, angularDeflection, inParallel));
    for (TopExp_Explorer fx(shape, TopAbs_FACE); fx.More(); fx.Next()) {
        const TopoDS_Face& f = TopoDS::Face(fx.Current());
        TopLoc_Location loc;
        if (!BRep_Tool::Triangulation(f, loc).IsNull()) continue;
        try {
            IMeshTools_Parameters wp = meshParams(deflection, angularDeflection, false);
            wp.MeshAlgo = IMeshTools_MeshAlgoType_Watson;
            BRepMesh_IncrementalMesh(f, wp);
        } catch (...) {
            // Fall through to the escalating retry below.
        }
        if (BRep_Tool::Triangulation(f, loc).IsNull())
            meshBareFaceEscalating(f, deflection, angularDeflection);
    }
}

} // namespace materializr
