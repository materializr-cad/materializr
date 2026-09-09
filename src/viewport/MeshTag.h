#pragma once

#include <BRep_Tool.hxx>
#include <Poly_Triangulation.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>

#include <cmath>

namespace materializr {

// What the mesher achieved on a TShape, recorded when it ran. ShapeRenderer
// keeps one per TShape to decide whether tessellate() may skip the mesher.
// OCCT itself stores only the ACHIEVED deflection on each Poly_Triangulation
// (0 on a plane), never the requested one, so the request is kept here.
struct MeshTag {
    float deflection = 0.0f;        // requested linear deflection
    float angularDeflection = 0.0f; // requested angular deflection
    int faces = 0;                  // faces in the shape
    int unmeshedFaces = 0;          // faces the mesher left without a triangulation
};

// Faces of `shape` without a triangulation; `total` gets the face count.
inline int countUnmeshedFaces(const TopoDS_Shape& shape, int* total = nullptr)
{
    int bare = 0, all = 0;
    for (TopExp_Explorer fx(shape, TopAbs_FACE); fx.More(); fx.Next()) {
        ++all;
        TopLoc_Location l;
        if (BRep_Tool::Triangulation(TopoDS::Face(fx.Current()), l).IsNull()) ++bare;
    }
    if (total) *total = all;
    return bare;
}

inline MeshTag makeMeshTag(const TopoDS_Shape& shape, float deflection,
                           float angularDeflection)
{
    MeshTag t;
    t.deflection = deflection;
    t.angularDeflection = angularDeflection;
    t.unmeshedFaces = countUnmeshedFaces(shape, &t.faces);
    return t;
}

// Whether `tag` still describes `shape` meshed at exactly these parameters:
// same request (a different value never matches, in either direction, so a
// finer mesh cannot survive a quality lowering), same face count, and exactly
// the faces the mesher could not triangulate are still bare. A face the
// mesher cannot triangulate (a self-intersecting wire, some fused tangent
// surfaces) therefore no longer forces a Clean and a full re-mesh on every
// rebuild. A shape with EVERY face bare is never covered: a fresh TShape at a
// recycled address looks exactly like that, and must be meshed.
inline bool meshTagCovers(const MeshTag& tag, float deflection, float angularDeflection,
                          const TopoDS_Shape& shape)
{
    constexpr float kSameDeflection = 1e-6f; // exact match, not "close enough"
    if (std::abs(tag.deflection - deflection) >= kSameDeflection ||
        std::abs(tag.angularDeflection - angularDeflection) >= kSameDeflection)
        return false;
    int faces = 0;
    const int bare = countUnmeshedFaces(shape, &faces);
    return faces == tag.faces && bare == tag.unmeshedFaces && bare < faces;
}

} // namespace materializr
