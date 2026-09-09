#pragma once
#include "../core/Operation.h"
#include "../core/Document.h"
#include "TopoName.h"
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Vec.hxx>
#include <gp_Dir.hxx>
#include <gp_Trsf.hxx>
#include <vector>
#include <string>

// Slide a face WITHIN ITS OWN PLANE (lateral move, never along the normal -
// that's PushPull). The whole body shears to follow: the selected face's plane
// shifts by the in-plane move vector, the opposite end stays pinned, linear in
// between - so a box's top slid sideways becomes a parallelepiped with slanted
// walls, and any other features lean proportionally. Implemented as one affine
// gp_GTrsf shear (BRepBuilderAPI_GTransform): no booleans, topology always
// valid by construction. The move vector is projected onto the face plane.
class MoveFaceOp : public Operation {
public:
    MoveFaceOp() = default;
    ~MoveFaceOp() override = default;

    // The face transform this op applies - same loft engine, different motion:
    //   Translate: slide the face in its plane (Move).
    //   Rotate:    tilt the face about an in-plane axis through its centre (Taper).
    //   Scale:     grow/shrink the face about its centre (Scale Face).
    //   Twist:     spin the face about its NORMAL relative to the opposite cap,
    //              so the walls spiral. Unlike the others this is a LAYERED loft
    //              (base -> N intermediate rotated sections -> top) because a
    //              single ruled loft re-aligns wires and caps out at ~45deg; the
    //              small per-step rotation keeps corner correspondence honest for
    //              any total angle (see probe_twist_face).
    enum class Kind { Translate, Rotate, Scale, Twist };

    void setBody(int bodyId) { m_bodyId = bodyId; }
    void setFace(const TopoDS_Face& f) { m_face = f; }
    void setKind(Kind k) { m_kind = k; }
    // The full translation (direction * distance) applied to the face (Translate).
    void setMoveVector(const gp_Vec& v) { m_move = v; }
    // Rotate: tilt axis direction (in the face plane) + angle (radians), pivoting
    // at the face centre.
    void setRotation(const gp_Dir& axisDir, double angleRad) {
        m_rotAxis = axisDir; m_rotAngle = angleRad; m_rotUseExplicit = false;
    }
    // Rotate: an explicit composed rotation (already about the pivot) - lets the
    // UI stack tilts about both axes (5° right then 10° forward) into one op.
    void setRotationExplicit(const gp_Trsf& t) {
        m_rotExplicit = t; m_rotUseExplicit = true;
    }
    // Twist: rotation angle (radians) of the selected face about its normal,
    // relative to the opposite cap. The walls spiral between them.
    void setTwist(double angleRad) { m_twistAngle = angleRad; }
    // Scale: uniform factor about the face centre.
    void setScaleFactor(double f) { m_scaleFactor = f; m_scaleNonUniform = false; }
    // Scale: NON-uniform - separate factors along two in-plane axes (about the
    // centre). Built as a gp_GTrsf applied to the moving loops.
    void setScaleNonUniform(const gp_Dir& axisA, const gp_Dir& axisB,
                            double sA, double sB) {
        m_scaleNonUniform = true;
        m_scaleAxisA = axisA; m_scaleAxisB = axisB; m_scaleA = sA; m_scaleB = sB;
    }
    // Sketches lying ON the moved face - they slide with it (translated by the
    // in-plane move vector) as part of the same atomic op.
    void setSketchIds(std::vector<int> ids) { m_sketchIds = std::move(ids); }
    // Per-loop motion (three hole states, set by how much of the hole is
    // selected). moveOuter = the face outline slides. Per hole i (face-wire
    // order): holeSlant[i] = its TOP ring follows (top edge picked → slants),
    // holeVertical[i] = BOTH rings follow (cylinder wall picked → straight tube).
    // Neither = the hole stays put while the face moves around it. So:
    //   top ring slides    ⟺ holeSlant[i] OR holeVertical[i]
    //   bottom ring slides ⟺ holeVertical[i]
    void setLoopMotion(bool moveOuter, std::vector<bool> holeSlant,
                       std::vector<bool> holeVertical) {
        m_moveOuter = moveOuter;
        m_holeSlant = std::move(holeSlant);
        m_holeVertical = std::move(holeVertical);
    }

    int getBodyId() const { return m_bodyId; }
    gp_Vec getMoveVector() const { return m_move; }
    const TopoDS_Shape& getPreviousShape() const { return m_previousShape; }

    bool execute(Document& doc) override;
    bool undo(Document& doc) override;
    std::string name() const override { return "Move Face"; }
    std::string description() const override;
    void renderProperties() override;
    std::string typeId() const override { return "moveface"; }
    OperationDiff captureDiff() const override;
    std::vector<int> plannedBodyIds() const override { return {m_bodyId}; }
    std::string serializeParams() const override;
    bool deserializeParams(const std::string& blob) override;
    bool rehydrateFromReload(const ReloadState& state, Document& doc) override;

private:
    int m_bodyId = -1;
    TopoDS_Face m_face;
    Kind m_kind = Kind::Translate;
    gp_Vec m_move{0.0, 0.0, 0.0};
    gp_Dir m_rotAxis{1.0, 0.0, 0.0};
    double m_rotAngle = 0.0;       // radians (Rotate)
    gp_Trsf m_rotExplicit;         // composed rotation (Rotate, explicit mode)
    bool m_rotUseExplicit = false;
    double m_twistAngle = 0.0;     // radians about the face normal (Twist)
    double m_scaleFactor = 1.0;    // uniform (Scale)
    bool m_scaleNonUniform = false;
    gp_Dir m_scaleAxisA{1.0, 0.0, 0.0}, m_scaleAxisB{0.0, 1.0, 0.0};
    double m_scaleA = 1.0, m_scaleB = 1.0;
    std::vector<int> m_sketchIds; // sketches that ride along (translated by m_move)
    bool m_moveOuter = true;            // does the face outline slide?
    std::vector<bool> m_holeSlant;     // per-hole: top ring follows (slant)
    std::vector<bool> m_holeVertical;  // per-hole: both rings follow (straight tube)
    gp_Trsf m_appliedXform; // transform applied to on-face sketches (for undo)
    TopoDS_Shape m_previousShape; // for undo
    TopoDS_Shape m_resultShape;
    // Ordinal index of m_face within the pre-op body shape, for reload
    // (SubShapeIndex.h). Empty/unresolved → the step replays as a ReplayOp.
    std::vector<int> m_faceIndices;
    // Topological name of the target face - minted on the first execute, then
    // re-resolved whenever m_face has gone stale (an upstream edit rebuilt the
    // body and MOVED the face). Sketch-anchored, so it follows the move.
    // Serialized additively as `faceref=`; absent in old files.
    materializr::topo::Ref m_faceRef;
};
