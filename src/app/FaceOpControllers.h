#pragma once
#include "InteractiveOpController.h"
#include "CylindricalPick.h"
#include "MoveFaceState.h"

// Global scope, like the other modeling ops — forward-declared for
// configureFaceOp's signature so this header stays cheap to include.
class MoveFaceOp;
#include <TopoDS_Face.hxx>
#include <gp_Trsf.hxx>
#include <TopoDS_Shape.hxx>
#include <glm/glm.hpp>
#include <vector>
#include <cmath>

namespace materializr {

// ─── Shell ───────────────────────────────────────────────────────────────────
// Hollow a body, removing the picked face; thickness in the popup.
class ShellController : public InteractiveOpController {
protected:
    const char* title() const override { return "Shell"; }
    int onBegin(const IopContext& ctx) override;
    std::unique_ptr<Operation> buildOp(const IopContext& ctx) override;
    void panelBody(const IopContext& ctx, bool& changed) override;
    void onCleanup() override;
    float panelWidth() const override { return 300.0f; }

private:
    TopoDS_Face m_face;
    float m_thickness = 1.0f;
    char m_inputBuf[32] = "1.0";
    bool m_inputFocus = true;
};

// ─── Draft (TaperOp) ─────────────────────────────────────────────────────────
// Tilt the picked face(s) by an angle about a fixed neutral plane — OCCT's
// BRepOffsetAPI_DraftAngle, the moulding-draft operation. Named "Taper"
// internally (class, ToolAction, typeId) because the on-disk key is "taper";
// only the user-facing wording is "Draft", which is what the manufacturing
// world calls it and what stops it reading as a weaker Rotate.
class TaperController : public InteractiveOpController {
protected:
    const char* title() const override { return "Draft"; }
    int onBegin(const IopContext& ctx) override;
    std::unique_ptr<Operation> buildOp(const IopContext& ctx) override;
    void panelBody(const IopContext& ctx, bool& changed) override;
    void onCleanup() override;

private:
    bool resolveFrame(const IopContext& ctx, glm::vec3& dirOut,
                      glm::vec3& neutralOut) const;

    std::vector<TopoDS_Face> m_faces;
    float m_angle = 10.0f;     // degrees
    int   m_axisIdx = 0;       // 0=Auto, 1=X, 2=Y, 3=Z (user convention)
    bool  m_flipBase = false;
};

// ─── Project Sketch ──────────────────────────────────────────────────────────
// ─── Remove Face (defeature) ─────────────────────────────────────────────────
// Remove the picked face(s) and heal the surrounding faces back together —
// e.g. take a baked fillet/chamfer back to a sharp edge so it can be re-applied,
// or clean an unwanted round/hole off an imported part.
class DefeatureController : public InteractiveOpController {
protected:
    const char* title() const override { return "Repair Geometry"; }
    int onBegin(const IopContext& ctx) override;
    std::unique_ptr<Operation> buildOp(const IopContext& ctx) override;
    void panelBody(const IopContext& ctx, bool& changed) override;
    void onCleanup() override;
    float panelWidth() const override { return 280.0f; }

private:
    std::vector<TopoDS_Face> m_faces;
};

// ─── Project Sketch ──────────────────────────────────────────────────────────
// Project a sketch onto the picked face along the sketch normal, then
// engrave or emboss the projected regions — text wrapped onto a cylinder.
class ProjectSketchController : public InteractiveOpController {
protected:
    const char* title() const override { return "Projection"; }
    int onBegin(const IopContext& ctx) override;
    std::unique_ptr<Operation> buildOp(const IopContext& ctx) override;
    void panelBody(const IopContext& ctx, bool& changed) override;
    void onCleanup() override;
    float panelWidth() const override { return 300.0f; }
    bool wantsLivePreview(const IopContext& ctx) const override;

    // Past this many projected regions the live preview is dropped (the
    // per-change boolean would freeze the UI); Confirm still applies it. Set
    // low deliberately — it got slow around ~30 regions on high-end hardware,
    // so weaker machines need the cutoff earlier.
    static constexpr int kPreviewRegionCap = 20;

private:
    int effectiveRegionCount(const IopContext& ctx) const;
    TopoDS_Face m_face;
    std::vector<int> m_sketchIds;   // combo choices, built at begin
    int  m_sketchPick = 0;          // index into m_sketchIds
    std::vector<int> m_regionFilter; // region subset from selection; empty = all
    float m_depth = 1.0f;
    int   m_mode = 0;               // 0=Engrave, 1=Emboss
    int   m_cycleMode = 0;          // 0=all, 1=loops only, 2=islands only
};

// ─── Scale Face ──────────────────────────────────────────────────────────────
// Pinch/flare the body toward a scaled copy of a planar END face. Carries
// the 2D gizmo frame the viewport draws and drags (red U / blue V arrows).
class ScaleFaceController : public InteractiveOpController {
protected:
    const char* title() const override { return "Scale Face"; }
    // The gizmo frame used to be public (center/axisU/halfU/dragAxis/…) purely
    // so Application_Viewport could run the hit-test and drag from outside.
    // Both now live here, so the frame is private again.
    bool wantsViewportInput() const override { return true; }
    void onViewportInput(const IopViewport& vp, const IopContext& ctx) override;
    void drawOverlay(const IopOverlay& ov) const override;
    int onBegin(const IopContext& ctx) override;
    std::unique_ptr<Operation> buildOp(const IopContext& ctx) override;
    void panelBody(const IopContext& ctx, bool& changed) override;
    void onCleanup() override;

private:
    // Drag delta (percent) onto one axis; respects the Uniform link.
    void applyHandleDrag(int axis, float dPct, const IopContext& ctx);
    // Scale ceiling, one place. 200% both ways now that growing unions the
    // frustum on instead of intersecting it (ScaleFaceOp::execute).
    float maxPct() const { return 200.0f; }

    TopoDS_Face m_face;
    float m_pctU = 30.0f;
    float m_pctV = 30.0f;
    bool  m_uniform = true;
    float m_len = 10.0f;
    float m_lenMax = 100.0f;
    // No mode member any more: the panel always re-slopes the existing walls
    // and the percentage decides shrink vs grow. ScaleFaceOp still HAS both
    // modes so saved projects that recorded Extend replay exactly as before.

    glm::vec3 m_center{0.0f};
    glm::vec3 m_axisU{1.0f, 0.0f, 0.0f};
    glm::vec3 m_axisV{0.0f, 0.0f, 1.0f};
    float m_halfU = 10.0f;
    float m_halfV = 10.0f;
    int   m_dragAxis = -1;
};

// ─── Resize Cylindrical (Edit Diameter) ──────────────────────────────────────
// Retarget a closed cylindrical face's diameter, or one circular END of it —
// editing a single end turns the cylinder into a cone, which is how funnels
// get made. Resolves its own target from the selection via
// detectCylindricalPick, so nothing has to hand it geometry.
class ResizeCylindricalController : public InteractiveOpController {
protected:
    const char* title() const override { return "Edit Diameter"; }
    int onBegin(const IopContext& ctx) override;
    std::unique_ptr<Operation> buildOp(const IopContext& ctx) override;
    void panelBody(const IopContext& ctx, bool& changed) override;
    void onCleanup() override;
    // A threaded body would re-run the ring boolean against the thread's
    // helicoid faces on every keystroke. Skip the live preview there and let
    // the base apply it once on Confirm; History reflows it beneath the Thread
    // step and the thread re-cuts in the background at the new radius.
    bool wantsLivePreview(const IopContext& ctx) const override;
    // ...but the commit stays INLINE. Deferring it moved the push out from
    // under the thread-reflow hook, which is the shape of the partially
    // re-cut thread Steve hit (a seam where only some turns took the new
    // diameter). The old code pushed synchronously; so does this.
    bool wantsDeferredCommit(const IopContext&) const override { return false; }

private:
    // True while editing BOTH ends (a face pick) — one field drives both.
    bool both() const { return m_pick.editBottom && m_pick.editTop; }
    // Has the user asked for a size different from what was picked? Distinct
    // from previewOk(): an unchanged value builds no op, which is not an error.
    bool changedFromOriginal() const {
        const double b = m_pick.editBottom ? m_newBottomDiameter * 0.5 : m_pick.bottomR;
        const double t = m_pick.editTop    ? m_newTopDiameter    * 0.5 : m_pick.topR;
        return std::abs(b - m_pick.bottomR) > 1e-5 ||
               std::abs(t - m_pick.topR)    > 1e-5;
    }

    CylindricalPick m_pick;
    double m_newBottomDiameter = 0.0;
    double m_newTopDiameter    = 0.0;
    char   m_botBuf[32] = "0.0";
    char   m_topBuf[32] = "0.0";
    bool   m_inputFocus = true;
    bool   m_deferred   = false;   // threaded body: no live preview
};

// ─── Move Face ───────────────────────────────────────────────────────────────
// Slide / tilt+twist / scale a face, with the body re-shaping to follow, plus
// the hole sub-modes (Slide, Tilt, EdgeMove) that reuse the same gizmo.
//
// MIGRATION IN PROGRESS (slice 2 of 3). The controller owns the state now;
// Application still holds a reference to it under its old name and still runs
// the lifecycle. That keeps ~520 existing references compiling while the logic
// moves across, instead of a single unreviewable jump. Slice 3 moves the gizmo
// draw + drag into drawOverlay/onViewportInput and drops the reference.
class MoveFaceController : public InteractiveOpController {
public:
    MoveFaceState& st() { return m_st; }
    const MoveFaceState& st() const { return m_st; }

    // The op-specific entry points (two ways in: a picked face, or a hole
    // recognised from its rim edges). begin() itself stays unused — this
    // controller predates the base's snapshot-preview model and keeps its
    // own lifecycle; the base-virtual overrides below route the GENERIC
    // call sites (Esc/Enter chains, single-flight) into it.
    void beginMoveFace(const IopContext& ctx, FaceXform kind);
    bool beginMoveHoleFromEdges(const IopContext& ctx);
    void updateMoveFace(const IopContext& ctx);
    void commitMoveFace(const IopContext& ctx);
    void cancelMoveFace(const IopContext& ctx);
    void moveFaceSlideSketches(const IopContext& ctx, const glm::vec3& v);

    void update(const IopContext& ctx) override { updateMoveFace(ctx); }
    void commit(const IopContext& ctx) override { commitMoveFace(ctx); }
    void cancel(const IopContext& ctx) override { cancelMoveFace(ctx); }
    // The panel is NOT scaffold-shaped: the banner + value wells anchor to
    // the viewport window, so renderViewport calls renderMoveFacePanel where
    // that window is current. The scaffold hook stays silent.
    void renderPanel(const IopContext&) override {}
    void renderMoveFacePanel(const IopContext& ctx, float uiScale);
    bool wantsViewportInput() const override { return true; }

    // Gesture maths — pure functions of the state, so they moved first.
    bool faceXformNontrivial() const;
    glm::mat3 faceRotTotal() const;
    void bakeFaceRotationDrag();          // fold a released ring drag in
    void configureFaceOp(MoveFaceOp& op) const;
    // True when the current gesture is one the local rebuild can express: the
    // Local box is ticked and this is a tilt (not a twist, slide or scale).
    bool localTweakApplies() const;
    // The gesture as one rigid transform, for FaceTweakOp.
    gp_Trsf faceTweakTrsf() const;
    // Run the local rebuild against the snapshot. Returns false (and leaves the
    // body on its snapshot) when the engine refuses, recording why in the state
    // so the panel can say it.
    bool applyLocalTweak(const IopContext& ctx);

    // The face gizmo (slice 3). Checks its own active flag — this controller
    // isn't in m_iops yet, so Application_Viewport calls it unconditionally.
    void drawGizmos3D(const IopGizmo3D& g) const override;
    // The drag (slice 4): ring/arrow latch on drag start, then slide / ring
    // sweep / twist / scale tracking. Rebuild is deferred to release — only
    // the ghost silhouette (drawOverlay below) moves mid-drag.
    void onViewportInput(const IopViewport& vp, const IopContext& ctx) override;
    // Ghost silhouette: each moving face loop as a yellow outline under the
    // current gesture transform.
    void drawOverlay(const IopOverlay& ov) const override;

protected:
    const char* title() const override { return "Move Face"; }
    int onBegin(const IopContext&) override { return -1; }  // slice 2: not yet
    std::unique_ptr<Operation> buildOp(const IopContext&) override {
        return nullptr;
    }
    void panelBody(const IopContext&, bool&) override {}

private:
    MoveFaceState m_st;
};

} // namespace materializr
