#pragma once
#include "InteractiveOpController.h"
#include "HistoryEditPreview.h"
#include <TopoDS_Shape.hxx>
#include <glm/glm.hpp>
#include <vector>

namespace materializr {

enum class EdgeOpKind { None, Fillet, Chamfer };

// Re-resolve every fillet/chamfer op's generated-face mapping against the
// current bodies. Must run after ANY editStep replay (commit, cancel, or the
// zero-value bail) because the replay re-runs each op's execute(), leaving its
// faces at their pre-downstream-Transform positions until rebound. Also run
// once after a project load, which is why it is a free function rather than a
// controller method.
void refreshAllEdgeOpFaces(History& hist, Document& doc);

// Interactive Fillet / Chamfer.
//
// The one op that needs BOTH a create path and an edit path, on two different
// preview models - which is why it was extracted last:
//
//   CREATE  - plain PreviewModel::SnapshotBody. One body is snapshotted, a
//             transient FilletOp/ChamferOp runs against it per preview frame,
//             and commit pushes a real op. The base already does exactly this.
//   EDIT    - clicking a face an existing fillet produced re-opens that step.
//             The preview cannot be transient: downstream ops (a chamfer
//             stacked on this fillet, a cut through it) have to recompute or
//             they flicker out for the drag. So the preview mutates the real
//             op's parameter and replays through History::editStep, guarded by
//             HistoryEditPreview. See that header for the two hazards.
//
// The mode is fixed at begin (editingIndex() >= 0 means edit) and never
// changes mid-gesture, so previewModel() answering differently per mode is
// safe - begin/commit/cancel each read it once.
class EdgeOpController : public InteractiveOpController {
public:
    // Create: fillet/chamfer the selected edges. Returns false when nothing
    // usable is selected (or the selection is an imported mesh).
    bool beginEdgeOp(const IopContext& ctx, EdgeOpKind kind);
    // Edit: re-open the FilletOp/ChamferOp at `historyIndex`. `pickedBodyId` is
    // the body whose blend FACE was clicked - used only to detect a baked
    // feature (one whose geometry never changes) and say so.
    bool beginEdgeOpEdit(const IopContext& ctx, int historyIndex,
                         int pickedBodyId);

    // Re-run the preview at the current value(s). Returns true iff a non-zero
    // preview was successfully applied; begin uses that to probe a starting
    // radius so a fresh fillet shows something immediately.
    bool updateEdgeOp(const IopContext& ctx);

    // The value panel (banner + well + the chamfer's A/B controls). Called from
    // renderViewport where the viewport window is current, because it anchors
    // to that window's rect - same arrangement as Extrude and Push/Pull.
    void renderEdgeOpPanel(const IopContext& ctx);
    // Enter-to-confirm from the global key handler (no scaffold panel).
    void confirmFromKey(const IopContext& ctx);

    EdgeOpKind kind() const { return m_kind; }
    // The edge midpoint - Application latches the im-touch panel anchor to it.
    const glm::vec3& mid() const { return m_mid; }
    bool hasHandle() const { return m_hasHandle; }
    // A handle is claimed this drag; the viewport suppresses camera orbit.
    bool dragging() const { return m_dragging; }

    void update(const IopContext& ctx) override;
    void commit(const IopContext& ctx) override;
    void cancel(const IopContext& ctx) override;

protected:
    const char* title() const override {
        return m_kind == EdgeOpKind::Fillet ? "Fillet" : "Chamfer";
    }
    PreviewModel previewModel() const override {
        return m_editingIndex >= 0 ? PreviewModel::HistoryEdit
                                   : PreviewModel::SnapshotBody;
    }
    int onBegin(const IopContext& ctx) override;
    std::unique_ptr<Operation> buildOp(const IopContext& ctx) override;
    bool previewOffThread() const override { return true; }
    void panelBody(const IopContext& ctx, bool& changed) override;
    void markPreviewDirty(const IopContext& ctx) const override;
    // The panel is renderEdgeOpPanel (viewport-anchored), so the scaffold's
    // stays silent.
    void renderPanel(const IopContext&) override {}
    bool wantsViewportInput() const override { return true; }
    void onViewportInput(const IopViewport& vp, const IopContext& ctx) override;
    void drawOverlay(const IopOverlay& ov) const override;
    void onCleanup() override;

private:
    // Compute the two in-face drag directions for the first selected edge
    // (A = the face ChamferOp uses for distance 1). Sets m_hasFaceDirs and
    // m_canTwoDist. Requires m_edges + snapshot() + m_mid/m_dir already set.
    void computeFaceDirs();
    // Read the first edge's midpoint + tangent and the outward perpendicular
    // the arrow grows along. `outwardFromFaces` uses the adjacent faces'
    // averaged outward normals (correct on concave edges too); the edit path
    // passes false to keep its historical bbox-centre heuristic.
    void computeHandleFrame(bool outwardFromFaces);
    // Write the previewed value(s) into the history op being re-edited.
    void writeEditedParams(const IopContext& ctx, float v, float v2) const;
    // Shared teardown for every commit/cancel exit.
    void finish(const IopContext& ctx);

    EdgeOpKind m_kind = EdgeOpKind::None;
    std::vector<TopoDS_Shape> m_edges;
    // The body onBegin will return - captured by the two entry points, which
    // resolve it from the selection (create) or the op (edit).
    int m_pendingBody = -1;
    // EDIT only: the edited op's own getPreviousShape(), which is its
    // pre-state - NOT the current body. onBegin installs it as the snapshot so
    // the handle frame and the chamfer face directions are computed against
    // the geometry the op was originally applied to.
    TopoDS_Shape m_editPreShape;
    float m_value = 1.0f;
    char m_inputBuf[32] = "1.0";
    bool m_inputFocus = true;

    // First selected edge's midpoint + direction, for the drag handle and the
    // radius/distance readout.
    glm::vec3 m_mid{0.0f};
    glm::vec3 m_dir{1.0f, 0.0f, 0.0f};    // along the edge
    glm::vec3 m_outDir{0.0f, 0.0f, 1.0f}; // perpendicular, out of the body
    bool m_hasHandle = false;

    // Two-distance (asymmetric) chamfer: a second setback along the OTHER
    // adjacent face, dragged via a second arrow. m_faceDirA/B are the two
    // in-face drag directions (A = ChamferOp's reference face). m_grab latches
    // which arrow owns a drag: 0 = A (distance 1), 1 = B (distance 2).
    bool  m_twoDist = false;
    float m_value2 = 0.0f;
    char  m_inputBuf2[32] = "1.0";
    glm::vec3 m_faceDirA{0.0f, 0.0f, 1.0f};
    glm::vec3 m_faceDirB{0.0f, 1.0f, 0.0f};
    bool  m_hasFaceDirs = false;
    bool  m_canTwoDist = false;   // selection supports a consistent A/B chamfer
    int   m_grab = -1;

    // Set on the left-click frame iff the cursor was near the arrow line;
    // cleared on release. Without the click claim, trackpad-mode left-orbit
    // grabbed the drag-threshold frame and the arrows felt dead - and
    // conversely, dragging from empty space now orbits instead of yanking the
    // value. (Steve: chamfer/fillet arrows didn't grab the cursor in trackpad
    // mode.)
    bool  m_dragging = false;

    // ── Edit mode only ───────────────────────────────────────────────────────
    // History index of the op being re-edited; -1 means "creating new".
    int m_editingIndex = -1;
    // The body whose blend FACE was clicked. If its geometry doesn't change
    // after the edit, the op drives a different/deleted body and the clicked
    // geometry has no editable op behind it - we say so instead of silently
    // doing nothing.
    int m_pickedBodyId = -1;
    // That body's geometry BEFORE the first preview replay. Measured here
    // rather than at commit, where the preview has already moved the body to
    // the new radius and any comparison would read "unchanged".
    double m_prePickedVol  = 0.0;
    double m_prePickedArea = 0.0;
    // The value(s) the op had at edit-begin. Cancel (and the confirm-at-zero
    // "treat as cancel" path) restore them before replaying, since the edit
    // preview mutates the real op's parameter.
    float m_origValue = 0.0f;
    float m_origValue2 = 0.0f;
    HistoryEditPreview m_editPreview;
};

} // namespace materializr
