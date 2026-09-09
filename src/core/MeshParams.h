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

} // namespace materializr
