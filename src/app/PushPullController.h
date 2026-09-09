#pragma once
#include "InteractiveOpController.h"
#include "AsyncJob.h"
#include "PushPullDispatch.h"
#include "PushPullPreview.h"
#include "PushPullState.h"
#include "core/BodyChanges.h"

#include <glm/glm.hpp>
#include <memory>
#include <vector>

class PushPullOp;

namespace materializr {
class PreviewJob;

// Interactive Push/Pull: take the selected sketch regions and/or flat body
// faces and sweep them along the face normal - positive extrudes (fuse),
// negative cuts.
//
// Runs on PreviewModel::LiveOp, which it is the ancestor of: the base's live-op
// engine was lifted from the hand-written version of this op, after per-frame
// History push/undo produced a whole bug class here (body ids changing every
// frame, empty-document click windows, outside history touches desyncing the
// bookkeeping). Slice 2 is therefore mostly a matter of deleting that hand-
// rolled engine and letting the base drive.
//
// TWO things don't fit the plain LiveOp shape, and both land on hooks that
// already existed for Extrude:
//
//   * The GHOST path. A threaded rod has hundreds of helicoid faces, and
//     push/pull triggers the thread-last reflow - so a real boolean per drag
//     frame was "a no go, non-responsive ~10s". Those gestures suppress the
//     live preview (wantsLivePreview) and draw a tinted tool volume through
//     ctx.showGhost instead; buildCommitOp then runs the real op ONCE.
//   * The SMART CUT reroute. A free-space sketch, or any negative distance,
//     should cut every visible body in the tool's path - but the preview
//     always showed the plain new-body extrusion. buildCommitOp hands back a
//     cut-enabled op, and the base undoes the preview before pushing it.
class PushPullController : public InteractiveOpController {
public:
    // Entry point: scans the CURRENT selection (sketch regions, flat body
    // faces, whole sketches from the Items panel). Returns false when nothing
    // usable was selected - or when the only picks were curved faces, which it
    // toasts about (#28).
    bool beginPushPull(const IopContext& ctx);

    // Re-run the preview at the current distance. applySnap=false keeps a typed
    // value exact (the grid step would round it under the user).
    void updatePushPull(const IopContext& ctx, bool applySnap = true);

    // The distance panel (banner + value well + Symmetric + Confirm/Cancel).
    // Called from renderViewport where the viewport window is current, because
    // it anchors to that window's rect - same arrangement as Extrude's.
    void renderPushPullPanel(const IopContext& ctx);
    // Enter-to-confirm from the global key handler: take whatever is in the
    // text field, then commit. (This op has no scaffold panel to catch it.)
    void confirmFromKey(const IopContext& ctx);

    // The arrow's frame - Application still DRAWS the dimension arrow (shared
    // renderer with extrude + the edge ops).
    bool hasArrow() const { return m_st.hasArrow; }
    const glm::vec3& origin() const { return m_st.origin; }
    const glm::vec3& normal() const { return m_st.normal; }
    float distance() const { return m_st.distance; }
    // Trackpad click-move-click drag is engaged - the viewport suppresses
    // camera orbit while it is (gizmoOwnsDrag).
    bool sticky() const { return m_st.sticky; }
    // Off-thread preview (see PushPullDispatch): once one inline preview of
    // this gesture took kAsyncPreviewMs or more, later frames draw the ghost
    // and run the boolean on a worker. Application polls once per frame.
    void pollPreview(const IopContext& ctx) override;
    bool previewPending() const override; // a worker job is running: keep frames coming

    // Public because the base's is: the generic Esc chain and single-flight
    // cancellation call it. Overridden only to drop the ghost mesh first -
    // it is renderer-only, so nothing else would.
    void cancel(const IopContext& ctx) override;
    // The preview job (AsyncJob) joins every worker it started, running or
    // abandoned: a worker still inside OCCT while statics tear down was the
    // one hazard detached threads had.
    ~PushPullController() = default;

protected:
    const char* title() const override { return "Push / Pull"; }
    PreviewModel previewModel() const override { return PreviewModel::LiveOp; }
    int onBegin(const IopContext& ctx) override;
    std::unique_ptr<Operation> buildOp(const IopContext& ctx) override;
    bool syncLiveOp(Operation& op) override;
    std::unique_ptr<Operation> buildCommitOp(const IopContext& ctx) override;
    // Dense bodies draw a ghost instead of previewing for real.
    bool wantsLivePreview(const IopContext&) const override {
        return !m_st.heavyPreview;
    }
    // A ghosted gesture never previewed for real, so its boolean runs in full
    // at commit - 814 ms on a 300-hole plate, on the main thread. Run it
    // between frames instead, where PushPullOp's progress range keeps the
    // window painting and Cancel aborts the boolean.
    //
    // Never on a threaded body. Application's main loop applies landed thread
    // re-cuts (pollThreadRecuts) near the top of an iteration and runs the
    // deferred task later in the same one, so a re-cut can land between the
    // commit frame and the push and change the body underneath it. History
    // also has to reflow this op beneath the Thread step; moving the push out
    // from under that hook is what produced a partially re-cut thread before
    // (see ResizeCylindricalController, which stays inline for the same
    // reason).
    //
    // The threaded check is re-run HERE rather than read from m_st, which
    // holds what was true at gesture start. The Items panel renders during the
    // gesture and its visibility checkbox is live, so a hidden threaded body
    // can be revealed mid-drag - and cutVisibleBodies reads visibility when it
    // executes, not when the gesture began. Trusting the stale answer would
    // defer a commit that then cuts the very body this exclusion exists for.
    bool wantsDeferredCommit(const IopContext& ctx) const override;
    // Any VISIBLE body carrying a thread, asked fresh. Static: it reads only
    // the context, never gesture state.
    static bool anyVisibleBodyThreaded(const IopContext& ctx);
    void markPreviewDirty(const IopContext& ctx) const override;
    void panelBody(const IopContext& ctx, bool& changed) override;
    // The panel is renderPushPullPanel (viewport-anchored), so the scaffold's
    // stays silent.
    void renderPanel(const IopContext&) override {}
    bool wantsViewportInput() const override { return true; }
    void onViewportInput(const IopViewport& vp, const IopContext& ctx) override;
    void onCleanup() override;

private:
    // Build a PushPullOp from the current targets + distance. Used for the live
    // instance, for the ghost path's single real execute, and for the smart-cut
    // reroute.
    std::unique_ptr<PushPullOp> makeOp() const;
    // Every target is a free-space sketch region (no host body) - the case that
    // should cut through whatever it runs into rather than make an overlapping
    // new body.
    bool allFreeSketchTargets() const;
    // Draw (or drop) the tinted tool volume for the heavy path.
    void updateGhost(const IopContext& ctx) const;
    // Fold a viewport drag delta into the unsnapped accumulator.
    void applyDrag(const IopViewport& vp);

    // Async preview: start a worker job at the current distance if the
    // dispatch says so.
    void launchPreviewIfWanted(const IopContext& ctx);
    PushPullState m_st;
    PushPullDispatch m_ppDispatch;
    BodySnapshot m_originals; // bodies as they were when the gesture began
    AsyncJob<PreviewResult> m_ppJob; // the worker job in flight, if any
};

} // namespace materializr
