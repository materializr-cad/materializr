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
//   * every edge LEAVING the moved     -> unchanged geometry (neither of ITS
//     face into the body                  faces moved), retrimmed to a new end.
//                                         A box's vertical edges keep their
//                                         lines and simply shorten.
//   * every vertex ON the moved face   -> where that leaving edge now crosses
//                                         the moved surface.
//   * every edge ON the moved face     -> the moved surface intersected with
//                                         its one neighbour, trimmed between
//                                         those vertices.
//   * everything else                  -> untouched, which is the entire point.
//
// Solving the VERTEX off the leaving edge rather than off three surfaces is
// what lets the neighbours curve. The three-plane version this started as
// needed every face at a corner to be a plane; a curve crossing a plane needs
// nothing of the sort, and it answers the awkward corners for free — the seam
// vertex where a bore meets a face has only TWO faces at it, not three, so
// there was never a third plane to solve with.
//
// SCOPE OF THIS VERSION. The moved face itself must be PLANAR; its neighbours
// need not be. Each of its corners must have exactly one edge leaving into the
// body — an ordinary manifold corner — and each rebuilt edge's new curve has to
// come out of the kernel as a real intersection. Everything else is refused
// explicitly, with a reason the UI can print, rather than silently producing a
// mangled body. A curved MOVED face is the next step and a bigger one: the
// corner solve becomes curve-against-curved-surface and the moved face itself
// needs a new trimmed surface, not just a new plane.
namespace materializr::tweak {

// Why a tweak was refused. Every one of these is a case the caller should be
// able to explain to the user, not a generic failure.
enum class Refusal {
    None,
    FaceNotFound,        // the face isn't part of that body
    NotPlanar,           // the moved face is curved — see SCOPE above
    NonManifoldCorner,   // a corner with no single edge leaving into the body
    NoChange,            // the move slid the face inside its own plane
    Degenerate,          // an edge or surface no longer meets the moved face
    NoIntersection,      // a neighbour's surface doesn't cross the new plane
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
// rigid transform: the face is planar, so it stays planar under all of them.
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
