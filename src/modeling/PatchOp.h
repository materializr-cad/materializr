#pragma once
#include "../core/Operation.h"
#include "../core/Document.h"
#include <GeomAbs_Shape.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <string>
#include <vector>

// Patch — an N-sided surface fitted to a ring of picked edges, optionally
// constrained to run TANGENT (G1) or CURVATURE-CONTINUOUS (G2) into the faces
// on the other side of those edges.
//
// This is the "fill the void and blend it in" tool: pick the edges bounding a
// gap (a hole left by a deleted face, an opening in an imported shell, the
// notch between two bodies) and a single smooth face is fitted across it.
// With continuity above C0 the patch doesn't just plug the hole, it flows out
// of the surrounding surfaces without a visible crease.
//
// Engine: BRepOffsetAPI_MakeFilling (GeomPlate underneath) — a variational
// fit, not an interpolation. It minimises a bending energy subject to the
// boundary constraints, which is why the solver parameters below are exposed:
// they ARE the shape controls. In rough order of what a user reaches for:
//
//   * continuity   — C0 position only / G1 tangent / G2 curvature.
//   * nbPtsOnCur   — samples per boundary curve. The single biggest lever on
//                    how closely the patch tracks a wiggly boundary.
//   * degree       — degree of the initial surface the solver starts from.
//                    Higher = more freedom to bulge, and more chance of a
//                    wave the boundary never asked for.
//   * maxDeg /     — the approximation budget spent turning the plate into a
//     maxSegments    B-spline face. Too small and the tolerances below can't
//                    be met; too large and the face is heavy to mesh.
//   * tol3d / tolAng / tolCurv — how tightly G0 / G1 / G2 must actually be
//                    met. Loosening them is what rescues a fit that refuses.
//
// The achieved errors come back out of the solver (g0Error() etc.) so the
// panel can report what the fit actually managed rather than what was asked.
//
// SUPPORT FACES. G1/G2 are meaningless without a neighbouring surface to be
// continuous WITH. Each boundary edge therefore needs a support face:
//   * a face the user explicitly selected alongside the edges wins,
//   * otherwise the edge's adjacent face on the target body is used,
//   * an edge with no support falls back to C0 for that edge alone (the rest
//     of the ring keeps its continuity).
//
// RESULT. When the picked edges bound an opening in one body, the patch is
// sewn back into it and the body becomes closed again — the void is filled in
// place, not covered by a separate object. When it can't close (edges from two
// different bodies, a ring that doesn't bound anything, a sew that leaves free
// edges) the patch is added as its own surface body instead, which is still
// the right answer for bridging geometry the user will sew up later.
class PatchOp : public Operation {
public:
    // What became of the attempt to sew the patch into its body. The user sees
    // a loose surface either way, and the three reasons want three different
    // things done next, so the panel has to be able to tell them apart:
    //   NoSingleBody   — the edges came from two bodies; pick one ring.
    //   BodyIsClosed   — the body has no opening. A hole that goes right
    //                    THROUGH cannot be closed by capping one end, which is
    //                    the case that reads as the tool ignoring you.
    //   SewFailed      — there was an opening and the patch would not join it.
    enum class Heal { Sewn, NoSingleBody, BodyIsClosed, SewFailed };

    // Continuity the patch is asked to hold along its boundary. Stored as a
    // small int in the parameter blob rather than the OCCT enum, whose values
    // are not part of any file format we control.
    enum class Continuity { Position = 0, Tangent = 1, Curvature = 2 };

    PatchOp();
    ~PatchOp() override = default;

    // ── Inputs ──
    // The boundary ring. Order does not matter; MakeFilling closes the wire
    // itself from the constraint set.
    void setEdges(const std::vector<TopoDS_Edge>& edges);
    // Body the edges came from. -1 = standalone (the patch becomes its own
    // body and no support faces are auto-discovered).
    void setBody(int bodyId);
    // Faces the user picked as explicit tangency supports. Optional; each is
    // matched to the boundary edges that lie on it.
    void setSupportFaces(const std::vector<TopoDS_Face>& faces);

    // ── Shape controls ──
    void setContinuity(Continuity c) { m_continuity = c; }
    Continuity continuity() const { return m_continuity; }

    struct Solver {
        int    degree      = 3;      // initial surface degree
        int    nbPtsOnCur  = 15;     // samples per boundary curve
        int    nbIter      = 2;      // solver iterations
        bool   anisotropic = false;  // let u/v densities differ
        double tol2d       = 1e-5;
        double tol3d       = 1e-4;   // G0: max gap to the boundary, mm
        double tolAng      = 0.01;   // G1: max tangency error, radians
        double tolCurv     = 0.1;    // G2: relative curvature error
        int    maxDeg      = 8;      // approximation budget
        int    maxSegments = 9;
    };
    void setSolver(const Solver& s) { m_solver = s; }
    const Solver& solver() const { return m_solver; }

    // ── Results of the last successful execute() ──
    double g0Error() const { return m_g0Error; }  // mm
    double g1Error() const { return m_g1Error; }  // radians
    double g2Error() const { return m_g2Error; }  // relative
    // True when the patch closed the target body back into a solid, rather
    // than being added as a standalone surface.
    bool healedIntoBody() const { return m_healed; }
    Heal healOutcome() const { return m_healOutcome; }
    // Edges that ended up with no support face, so their stretch of the
    // boundary is only C0 no matter what continuity was asked for.
    int unsupportedEdgeCount() const { return m_unsupported; }
    // Did the fit actually deliver the continuity that was asked for? False
    // means the surface is sound but only holds position — see the note on
    // near-perpendicular supports in the .cpp. Always true for C0.
    bool continuityAchieved() const { return m_continuityAchieved; }
    const TopoDS_Face& patchFace() const { return m_patchFace; }

    int getBodyId() const { return m_bodyId; }
    int edgeCount() const { return static_cast<int>(m_edges.size()); }

    // ── Operation ──
    bool execute(Document& doc) override;
    bool undo(Document& doc) override;
    std::string name() const override { return "Patch"; }
    std::string description() const override;
    void renderProperties() override;
    std::string typeId() const override { return "patch"; }
    std::string serializeParams() const override;
    bool deserializeParams(const std::string& blob) override;
    bool rehydrateFromReload(const ReloadState& state, Document& doc) override;
    OperationDiff captureDiff() const override;
    std::vector<int> plannedBodyIds() const override {
        return m_bodyId >= 0 ? std::vector<int>{m_bodyId} : std::vector<int>{};
    }
    bool ownsFace(const TopoDS_Shape& face) const override;
    void snapshotEditState() override;
    void restoreEditState() override;

private:
    // Resolve the boundary edges against `base`, so a replay onto a rebuilt
    // body constrains the CURRENT geometry rather than stale handles.
    std::vector<TopoDS_Edge> resolveEdges(const TopoDS_Shape& base) const;
    // Support face for `edge`: an explicitly-picked face that carries it,
    // else its adjacent face from `ancestors` (the target body's edge->face
    // map, built once per execute). Null when neither exists.
    TopoDS_Face supportFor(const TopoDS_Edge& edge,
                           const TopTools_IndexedDataMapOfShapeListOfShape& ancestors) const;
    // Sew the patch into `base` and close it back up. Null shape if the
    // result would not be an improvement on what was there.
    TopoDS_Shape healInto(const TopoDS_Shape& base, const TopoDS_Face& patch) const;

    // One MakeFilling attempt. `initFace` may be null (GeomPlate guesses its
    // own initial surface) or a face from an earlier attempt. Returns false if
    // the solver declined or the result failed validation; never throws.
    struct Fit {
        TopoDS_Face face;
        double g0 = 0.0, g1 = 0.0, g2 = 0.0;
        int unsupported = 0;
    };
    bool fitOnce(const std::vector<TopoDS_Edge>& edges,
                 const TopTools_IndexedDataMapOfShapeListOfShape& ancestors,
                 GeomAbs_Shape order, const TopoDS_Face& initFace, Fit& out) const;

    std::vector<TopoDS_Edge> m_edges;
    std::vector<TopoDS_Face> m_supports;
    int m_bodyId = -1;

    Continuity m_continuity = Continuity::Tangent;
    Solver m_solver;

    // Undo state. Exactly one of these is live: a healed patch modifies the
    // target body, a standalone patch creates one.
    TopoDS_Shape m_previousShape;
    int m_createdBodyId = -1;
    bool m_healed = false;

    Heal m_healOutcome = Heal::NoSingleBody;
    TopoDS_Face m_patchFace;
    double m_g0Error = 0.0, m_g1Error = 0.0, m_g2Error = 0.0;
    int m_unsupported = 0;
    bool m_continuityAchieved = true;

    // Transactional edit-state rollback (see Operation::snapshotEditState).
    struct EditSnap {
        std::vector<TopoDS_Edge> edges;
        std::vector<TopoDS_Face> supports;
        TopoDS_Shape previousShape;
        TopoDS_Face patchFace;
        bool healed = false;
        bool valid = false;
    };
    EditSnap m_editSnap;
};
