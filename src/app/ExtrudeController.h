#pragma once
#include "InteractiveOpController.h"
#include "../modeling/ExtrudeOp.h"
#include <TopoDS_Shape.hxx>
#include <glm/glm.hpp>
#include <vector>

namespace materializr {

// Interactive Extrude: sweep a sketch profile (or a flat body face) along its
// normal, with a draggable arrow and a distance well.
//
// FIRST user of the LiveOp preview model, and the reason that model got
// built. The hand-written version pushed a real ExtrudeOp onto History and
// undid it again on EVERY preview frame - the same churn Push/Pull was
// rescued from earlier, still carrying its symptoms: the created body's id
// changed each frame, and an outside history touch mid-gesture left the
// bookkeeping desynced (the old code had an explicit "preview op no longer on
// top of history - resyncing without undo" bail-out for exactly that). Keeping
// ONE instance and toggling it undo/execute fixes both: ExtrudeOp re-uses
// m_createdBodyId through addOrPutBody, so the preview body keeps its id, and
// History sees nothing at all until commit.
//
// The preview is ALWAYS a NewBody tool volume, even in Subtract mode, so the
// user watches the shape being swept; the real boolean runs once at commit
// through buildCommitOp().
class ExtrudeController : public InteractiveOpController {
public:
    // Entry point (this op is handed its profile rather than reading the
    // selection, so it doesn't fit onBegin's capture-from-selection shape).
    // Returns false when it refused - a curved face has no single normal.
    //
    // A Subtract may pass targetBody = -1: the body to cut is then resolved
    // from the swept volume at commit (see resolveCutTarget).
    bool beginExtrude(const IopContext& ctx, const TopoDS_Shape& profile,
                      ExtrudeMode mode, int targetBody, int sourceSketchId);

    // The arrow's frame - Application still DRAWS the dimension arrow (it
    // shares the extrude/push-pull/edge-op arrow renderer).
    const glm::vec3& origin() const { return m_origin; }
    const glm::vec3& normal() const { return m_normal; }
    float distance() const { return m_distance; }
    bool sticky() const { return m_sticky; }
    ExtrudeMode mode() const { return m_mode; }
    int previewBodyId() const;

    // The distance panel (banner + value well + Confirm/Cancel). Called from
    // renderViewport where the viewport window is current, because it anchors
    // to that window's rect - same arrangement as Move Face's.
    void renderExtrudePanel(const IopContext& ctx);
    // Enter-to-confirm from the global key handler: take whatever is in the
    // text field, then commit. (This op has no scaffold panel to catch it.)
    void confirmFromKey(const IopContext& ctx);

    // Re-run the preview at the current distance. applySnap=false keeps a
    // typed value exact (the grid step would round it under the user).
    void updateExtrude(const IopContext& ctx, bool applySnap = true);

    // Subtract resolves its target from the swept volume first, and REFUSES -
    // staying open, so the distance can be adjusted - when that volume reaches
    // no body at all. Committing anyway would either record a step that changed
    // nothing (a cut that misses leaves the body intact and passes every
    // validity check) or, with no target, leave the tool volume behind as a
    // stray new body.
    void commit(const IopContext& ctx) override;

protected:
    const char* title() const override { return "Extrude"; }
    PreviewModel previewModel() const override { return PreviewModel::LiveOp; }
    int onBegin(const IopContext& ctx) override;
    std::unique_ptr<Operation> buildOp(const IopContext& ctx) override;
    bool syncLiveOp(Operation& op) override;
    std::unique_ptr<Operation> buildCommitOp(const IopContext& ctx) override;
    void panelBody(const IopContext& ctx, bool& changed) override;
    // The panel is renderExtrudePanel (viewport-anchored), so the scaffold's
    // stays silent.
    void renderPanel(const IopContext&) override {}
    bool wantsViewportInput() const override { return true; }
    void onViewportInput(const IopViewport& vp, const IopContext& ctx) override;
    void onCleanup() override;

private:
    // Signed distance for the op: a Subtract's tool travels against the profile
    // normal (which points OUT of the host body) - except for a sketch with no
    // host, where the direction is aimed at the nearest body instead.
    double opDistance() const;
    // The visible body the current tool volume removes the most material from,
    // preferring m_targetBody. -1 when the sweep reaches nothing.
    int resolveCutTarget(const IopContext& ctx) const;
    // Every visible body the tool volume reaches, for the all-bodies option.
    std::vector<int> resolveAllCutTargets(const IopContext& ctx) const;
    // Push one Subtract per body. History has no op grouping - the existing
    // multi-body Boolean does the same - so each body's cut is its own step,
    // which also keeps each one's face lineage and undo exactly as they are for
    // the single-target case.
    void commitCutAll(const IopContext& ctx, const std::vector<int>& targets);

    TopoDS_Shape m_profile;
    ExtrudeMode m_mode = ExtrudeMode::NewBody;
    int m_targetBody = -1;
    // +1 sweeps along the profile normal, -1 against it. See opDistance.
    double m_sweepSign = 1.0;
    // Subtract only: cut EVERY body the sweep passes through, not just the one
    // it belongs to. Off by default - a sketch on a face means that face's
    // body, and silently carving a neighbour it happens to overlap would be a
    // surprise. Per-gesture, not persisted.
    bool m_cutAllBodies = false;
    int m_sketchId = -1;
    glm::vec3 m_normal{0, 0, 1};
    glm::vec3 m_origin{0};
    float m_distance = 5.0f;
    // Trackpad-mode click-move-click value drive (see Push/Pull).
    bool m_sticky = false;
    bool m_stickyPressWasDrag = false;
    char m_inputBuf[32] = "5.0";
    bool m_inputFocus = true;
};

} // namespace materializr
