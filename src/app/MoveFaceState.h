#pragma once
#include "../modeling/MoveHoleOp.h"
#include <TopoDS_Face.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Pln.hxx>
#include <glm/glm.hpp>
#include <vector>
#include <cmath>

namespace materializr {

// Rotation matrix (column-major glm) about a unit-ish axis by `angle` radians.
inline glm::mat3 rodrigues(const glm::vec3& axisIn, float angle) {
    glm::vec3 a = glm::normalize(axisIn);
    float c = std::cos(angle), s = std::sin(angle), t = 1.0f - c;
    float ax = a.x, ay = a.y, az = a.z;
    return glm::mat3(
        glm::vec3(c + ax*ax*t,      ay*ax*t + az*s,  az*ax*t - ay*s),   // col 0
        glm::vec3(ax*ay*t - az*s,   c + ay*ay*t,     az*ay*t + ax*s),   // col 1
        glm::vec3(ax*az*t + ay*s,   ay*az*t - ax*s,  c + az*az*t));     // col 2
}


// Which transform the face gizmo is applying. Was a nested enum on
// Application; it belongs with the state it describes.
enum class FaceXform { Translate, Rotate, Scale };

// Everything the Move Face tool holds while a gesture is live — the three
// interleaved transforms (slide / tilt+twist / scale), the silhouette ghost,
// the on-face sketches that ride along, and the hole sub-modes (Slide, Tilt,
// EdgeMove) that reuse the same gizmo.
//
// This was 35 separate members on Application, which is most of why the tool
// was hard to move: every one of them was reachable from anywhere in a 28k-line
// class. Collapsing them into one value changes no behaviour and is the step
// that makes the controller extraction a copy rather than a rewrite.
struct MoveFaceState {
    bool moveFaceActive = false;
    // Hole-move sub-mode of the Move tool: the same translate gizmo drives a
    // MoveHoleOp (slide a through-hole across its face) instead of a face shear.
    // Set when the Move selection is a recognizable hole wall (see beginMoveFace).
    bool moveHoleMode = false;
    // Which hole verb the current interactive move commits, and the rim side
    // being dragged when it's EdgeMove.
    MoveHoleOp::Mode moveHoleOpMode = MoveHoleOp::Mode::Slide;
    TopoDS_Edge moveHoleRimEdge;
    // Which mouth of the bore the user grabbed (buildVoid's own entry/exit
    // naming is unrelated to what was clicked — see MoveHoleOp::setNearIsEntry).
    bool moveHoleNearIsEntry = true;
    TopoDS_Face moveHoleWall;              // the clicked hole-wall seed face
    int  moveFaceBodyId = -1;
    TopoDS_Face  moveFaceFace;
    TopoDS_Shape moveFacePreviousShape;    // snapshot for preview / restore
    glm::vec3 moveFaceP0{0.0f};            // a point on the face plane
    glm::vec3 moveFaceN{0.0f, 0.0f, 1.0f}; // face plane normal (outward)
    glm::vec3 moveFaceVec{0.0f};           // accumulated in-plane slide
    glm::vec3 moveFaceBase{0.0f};          // slide banked before the current drag
    glm::vec3 moveFaceDragStart{0.0f};     // plane hit-point at drag start
    bool moveFaceDragging = false;
    // Two in-plane arrow axes + which one a drag latched (0=A, 1=B, -1=none).
    glm::vec3 moveFaceAxisA{1.0f, 0.0f, 0.0f};
    glm::vec3 moveFaceAxisB{0.0f, 1.0f, 0.0f};
    int  moveFaceGrab = -1;
    FaceXform faceXformKind = FaceXform::Translate;
    glm::vec3 moveFacePivot{0.0f};  // face centroid (rotate/scale pivot)
    float moveFaceAngle = 0.0f;     // accumulated tilt (radians, Rotate)
    float moveFaceAngleBase = 0.0f; // tilt banked before the current drag
    float moveFaceScale = 1.0f;     // accumulated uniform factor (Scale)
    float moveFaceScaleBase = 1.0f;
    // Non-uniform scale: separate factors along the two in-plane axes. When
    // uniform (default), both track moveFaceScale.
    bool  moveFaceScaleUniform = true;
    float moveFaceScaleA = 1.0f, moveFaceScaleB = 1.0f;
    float moveFaceScaleABase = 1.0f, moveFaceScaleBBase = 1.0f;
    glm::vec3 moveFaceRotAxis{1.0f, 0.0f, 0.0f}; // tilt axis latched this drag
    float moveFaceRotStartAngle = 0.0f; // cursor angle in the ring plane at drag start
    // Composed tilt from prior ring drags this session (about the fixed axes),
    // so you can stack 5° about one then 10° about the other. The live tilt is
    // rodrigues(rotAxis, angle) * accum; on each ring release the drag is baked
    // into accum and the angle resets.
    glm::mat3 moveFaceRotAccum{1.0f};
    bool moveFaceRotHasAccum = false;
    float moveFaceHalfExtent = 1.0f; // face size, maps drag distance → angle/scale
    bool  moveFaceRotSnap = true;    // snap tilt to whole degrees (default on)
    // TWIST = the THIRD rotation ring, about the face NORMAL (lies in the face
    // plane). Lives under FaceXform::Rotate: grabbing this ring (grab 2) spins
    // the face relative to its base and commits a MoveFaceOp::Kind::Twist —
    // distinct from the two tilt rings. Mutually exclusive with a tilt within a
    // session (moveFaceIsTwist picks which op the gesture builds).
    float moveFaceTwist = 0.0f;      // accumulated twist (radians) about the normal
    float moveFaceTwistBase = 0.0f;  // twist banked before the current drag
    float moveFaceTwistStart = 0.0f; // cursor angle in the face plane at drag start
    bool  moveFaceIsTwist = false;   // this Rotate gesture is a twist, not a tilt
    // DEFERRED REBUILD: the body rebuild is deferred to mouse-release, so the
    // drag only moves ghost SILHOUETTES of the face's loops. Loop 0 = outer
    // outline, 1..N = hole loops (same order as the op enumerates them). Each is
    // drawn translated by moveFaceVec only if that loop is flagged to move.
    std::vector<std::vector<glm::vec3>> moveFaceSilhouetteLoops;
    bool moveFacePendingRebuild = false;
    // Per-loop motion, derived from the SELECTION. moveOuter = a planar face is
    // selected (outline slides, holes slant). holeVertical[i] = that hole's
    // cylindrical face was Ctrl-selected → it moves as a straight tube. One per
    // hole, in loop order (matches moveFaceSilhouetteLoops[1..]).
    bool moveFaceMoveOuter = true;
    std::vector<bool> moveFaceHoleSlant;     // top edge picked → top ring follows
    std::vector<bool> moveFaceHoleVertical;  // cylinder wall picked → tube follows
    // LOCAL TILT. Route the gesture through the FaceTweak engine — which
    // rebuilds only the faces meeting this one — instead of MoveFaceOp's
    // whole-body GTransform shear. Tilt only: an in-plane slide has no local
    // answer at all (a plane translated along itself is the same plane), and
    // Scale/Twist have no three-plane formulation. See FaceTweak.h.
    bool moveFaceLocal = false;
    // Why the last local attempt declined, for the panel to print. Points at a
    // static string owned by tweak::refusalText; null = nothing to report.
    const char* moveFaceLocalRefusal = nullptr;
    // Sketches sitting ON the moved face — they slide with it. Original planes
    // snapshotted so the live preview / cancel can restore them.
    std::vector<int>    moveFaceSketchIds;
    std::vector<gp_Pln> moveFaceSketchPlanes0;
};

} // namespace materializr
