#pragma once
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Trsf.hxx>
#include <vector>

// Local face tweak — move a face and rebuild only what touches it.
//
// This is the operation Shapr and Plasticity get from their kernels and OCCT
// does not have. Parasolid calls it PK_FACE_tweak: hand it a face and a new
// surface and it re-intersects the neighbours, retrims their edges, and hands
// back a valid body. Nothing in OCCT does that, which is why MoveFaceOp reaches
// for BRepBuilderAPI_GTransform instead and shears the WHOLE body — a box whose
// top is slid sideways becomes a parallelepiped, every feature in it leaning
// proportionally. That is a fine modelling operation and a poor direct-editing
// one: the user grabbed one face and everything else moved.
//
// So: do the re-intersection ourselves.
//
// THE MODEL. A vertex of a solid is where three surfaces meet; an edge is where
// two do. Move one face's surface and those definitions still hold — they just
// resolve somewhere else. So the rebuild is:
//
//   * every vertex ON the moved face  -> re-solve as the intersection of the
//                                        moved surface with the two other faces
//                                        meeting there,
//   * every edge ON the moved face    -> the moved surface against its one
//                                        neighbour, trimmed between those
//                                        vertices,
//   * every edge LEAVING one of those -> unchanged geometry, retrimmed to the
//     vertices into the body             new endpoint. A box's vertical edges
//                                        keep their lines and simply shorten.
//   * everything else                 -> untouched, which is the entire point.
//
// SCOPE OF THIS VERSION. The moved face and every face meeting it must be
// planar, and each of its corners must be an ordinary three-face manifold
// corner. That covers boxes, brackets, plates and the flat regions of most
// parts, and it is refused explicitly — with a reason the UI can print —
// rather than silently producing a mangled body. Curved neighbours are the
// next step: the arithmetic below becomes GeomAPI_IntSS and the rebuilt faces
// need pcurves, but the shape of the algorithm does not change.
namespace materializr::tweak {

// Why a tweak was refused. Every one of these is a case the caller should be
// able to explain to the user, not a generic failure.
enum class Refusal {
    None,
    FaceNotFound,        // the face isn't part of that body
    NotPlanar,           // the moved face is curved
    NeighbourNotPlanar,  // something meeting it is curved — see SCOPE above
    NonManifoldCorner,   // a corner where more or fewer than three faces meet
    NoChange,            // the move slid the face inside its own plane
    Degenerate,          // the move made two planes parallel: no intersection
    OffCurve,            // a retrimmed edge's new end doesn't lie on it
    BuildFailed,         // a face, the sew, or the solid wouldn't build
    Invalid,             // it built and then failed validation
};

const char* refusalText(Refusal r);

struct Result {
    TopoDS_Shape shape;
    Refusal refusal = Refusal::None;
    bool ok() const { return refusal == Refusal::None && !shape.IsNull(); }
};

// Move `face` of `body` by `xf` and rebuild its neighbourhood. `xf` may be any
// rigid transform that leaves the face planar — an offset along the normal, a
// tilt, or a combination — which is what the three-plane corner solve needs.
//
// A slide INSIDE the face's own plane is refused as NoChange, and that is
// geometry, not a limitation: a planar face has no identity beyond its plane,
// so translating it within itself lands on the same plane and every corner
// re-solves exactly where it was. MoveFaceOp answers that gesture by shearing
// the whole body instead — a different, deliberate operation, and the reason
// both exist.
Result moveFace(const TopoDS_Shape& body, const TopoDS_Face& face,
                const gp_Trsf& xf);

} // namespace materializr::tweak
