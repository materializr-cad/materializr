#pragma once
#include "IopViewport.h"
#include <functional>
#include <memory>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Pln.hxx>

class Document;
class History;
class SelectionManager;
class Operation;

namespace materializr {

// Where a viewport-anchored panel should sit and at what scale. The app knows
// the layout; a controller only needs the numbers, so this crosses the
// boundary instead of imTouchLayout()/uiScale()/the action-anchor members.
struct IopPanelPlace {
    float uiScale = 1.0f;
    // im-touch layout: the panel collapses to a single tappable value well
    // (no header, hint or steppers).
    bool  imTouch = false;
    // im-touch only: anchor the panel beside the action's own geometry (the
    // arrow tip), like the sketch bubbles. False = the fixed top-right spot.
    bool  anchorValid = false;
    float anchorX = 0.0f;
    float anchorY = 0.0f;
};

// Everything an interactive-op controller needs from the app, without
// seeing the Application god-class. Built on demand by Application.
struct IopContext {
    Document& doc;
    History& history;
    SelectionManager& selection;
    std::function<void()> markMeshesDirty;
    // Long-op progress: renders a progress frame, returns true if the user
    // cancelled. Passed to the op via setProgressReporter on commit.
    std::function<bool(float, const char*)> progress;
    // Defer a heavy task to run BETWEEN frames (so the progress reporter can
    // render its own frames without nesting ImGui frames).
    std::function<void(std::function<void()>)> deferHeavy;
    // The layout hosts Confirm/Cancel outside the panel (im-touch corner
    // FABs) — renderPanel skips its own buttons; Enter/Esc still work.
    bool cornerCommitUi = false;

    // A transient message to the user. Controllers that refuse a selection
    // need to say why; the five panel-only ones never did, which is why this
    // wasn't here before Move Face.
    std::function<void(const char*)> toast;
    // "This selection is an imported mesh, so decline" — the shared guard from
    // core/MeshGuard.h, already wired with the app's toast. Returns true when
    // it refused, in which case the caller must not start.
    std::function<bool(const char* opName)> refuseMesh;
    // Grid snap, for gestures that quantise a drag (Move Face's slide).
    bool  snapToGrid = false;
    float gridStep = 0.0f;
    // Panel placement for controllers that render their own viewport-anchored
    // panel (Extrude, Move Face) rather than the scaffold's.
    IopPanelPlace panel;

    // ── Selection scanning ───────────────────────────────────────────────────
    // Needed by a controller that reads the SELECTION itself. Extrude is handed
    // its profile, so Push/Pull is the first to want these.
    //
    // Re-bind a reloaded sketch's source face: it isn't serialised, so without
    // this buildRegions() misses the holes the user sketched around.
    std::function<void(int sketchId)> ensureSketchSourceFace;
    // The visible body a sketch region lies flat ON, or -1. A plane sketch over
    // a face fuses/cuts that body in place instead of spawning an overlapping,
    // z-fighting solid.
    std::function<int(const TopoDS_Face& region, const gp_Pln& plane)>
        findBodyUnderRegion;

    // ── Mesh invalidation, per body ──────────────────────────────────────────
    // markMeshesDirty() re-tessellates every visible body — on a 100-body
    // project that is the difference between a smooth preview frame and an
    // unusable one. These two let a preview touch only what it changed.
    std::function<void(int bodyId)> markBodyDirty;
    // Does the renderer already hold a slot for this body? A preview that
    // CREATES bodies uses it to spot the ones that just appeared. (Restrict to
    // VISIBLE bodies at the call site — invisible ones never get a slot, so
    // they would otherwise be marked dirty every frame forever.)
    std::function<bool(int bodyId)> bodyHasRenderSlot;

    // ── Ghost preview ────────────────────────────────────────────────────────
    // Show `tool` as a tinted, renderer-only volume — no Document body, no
    // History — in place of running the real boolean. Push/Pull uses it for
    // dense bodies, where a per-frame boolean over a thread's helicoid faces
    // (and the thread reflow behind it) makes the drag unusable; the real op
    // then runs once, at commit. `cut` tints it as a subtraction.
    //
    // The controller builds the shape (that is modelling, and TopoDS_Shape
    // already crosses this line); the renderer slot and colour stay the app's.
    std::function<void(const TopoDS_Shape& tool, bool cut)> showGhost;
    std::function<void()> clearGhost;

    // The sketch that drives this body, or -1. Generative anchoring: a fillet
    // records it so a filleted corner can follow a later dimension edit.
    std::function<int(int bodyId)> sketchForBody;
};

// Base for "popup with live preview" modeling operations (Shell, Taper,
// Scale Face, …). Before this existed, every such feature deposited ~400
// lines across five Application files: a state block, four lifecycle
// methods, a panel function, a ToolAction case, an Esc-chain entry, and
// membership in TWO hand-maintained gizmo-suppression lists (forgetting a
// list entry was a recurring bug class). A controller subclass implements
// four small hooks; the lifecycle, the panel scaffold, and the
// suppression/Esc registration come from here.
//
// TWO preview models, because the ops genuinely differ (this was the finding
// that stalled the Extrude/Push-Pull extraction):
//
//   SnapshotBody — the original. One target body is snapshotted at begin();
//     every preview restores it and runs a FRESH op against it. Shell, Taper,
//     Scale Face, Projection, Defeature, Resize Cylindrical. Requires exactly
//     one pre-existing body to modify, so an op that CREATES a body can't use
//     it.
//
//   LiveOp — one op INSTANCE is kept for the whole gesture and toggled
//     against the document: undo() it, push the new parameter values in
//     (syncLiveOp), execute() it again. History is untouched until commit,
//     which records the already-applied instance via pushExecuted().
//     Handles body creation, multi-body edits, and — the reason it exists —
//     keeps created-body IDS STABLE across preview frames, because the same
//     instance re-uses the ids it minted (ExtrudeOp::m_createdBodyId,
//     PushPullOp's reuse pool). Push/Pull was hand-converted to exactly this
//     engine after the alternative (pushing and undoing real History steps
//     per frame) produced a whole bug class: ids changing every frame,
//     empty-document click windows, and outside history touches corrupting
//     the preview bookkeeping. The base now owns that engine so the next op
//     doesn't have to re-derive it.
//
//   HistoryEdit — the op is ALREADY on History and its parameter is the thing
//     being dragged: re-opening a committed fillet by clicking the face it
//     produced. Neither model above works. SnapshotBody would restore one body
//     and lose everything DOWNSTREAM of the step (a chamfer stacked on that
//     fillet flickers out for the length of the drag); LiveOp assumes an
//     unrecorded instance. So the preview IS the real replay — mutate the
//     step's parameter and call History::editStep. The controller owns
//     update/commit/cancel for this model (the policy is all op-specific:
//     which parameter, which failure message); the base only skips the other
//     two models' bookkeeping. The mechanism — whole-document snapshot,
//     edit-state snapshot, restore-on-failed-replay — is HistoryEditPreview.h.
//
// Lifecycle contract:
//   begin   — capture params from the selection (onBegin); snapshot the
//             target body (SnapshotBody only); run the first preview.
//   update  — SnapshotBody: restore the snapshot, build a fresh op, execute.
//             LiveOp: undo the live instance, syncLiveOp, execute it again.
//             previewOk() records whether it landed.
//   commit  — SnapshotBody: restore, rebuild, push onto History (which
//             re-executes it cleanly). LiveOp: hand the applied instance to
//             pushExecuted — unless buildCommitOp() offers a different op
//             (Extrude previews a NewBody tool volume but commits a boolean
//             cut), in which case the preview is undone and that op is
//             pushed normally. Clears the selection either way.
//   cancel  — undo the preview (restore the snapshot / undo the instance).
class InteractiveOpController {
public:
    virtual ~InteractiveOpController() = default;

    enum class PreviewModel { SnapshotBody, LiveOp, HistoryEdit };

    // onBegin() return value for a LiveOp controller with no single existing
    // body to snapshot — a free-space extrude CREATES its body, so there is
    // nothing to capture, but the op still starts. (-1 stays "refuse".)
    static constexpr int kNoTargetBody = -2;

    bool begin(const IopContext& ctx);
    // Virtual so a controller with a custom lifecycle (Move Face predates the
    // base's snapshot-preview model) can route the GENERIC call sites — the
    // Esc/Enter chains, single-flight cancellation — into its own logic.
    virtual void update(const IopContext& ctx);
    virtual void commit(const IopContext& ctx);
    virtual void cancel(const IopContext& ctx);

    // Draws nothing when inactive. The scaffold handles window placement,
    // title, Confirm/Cancel buttons, and Enter/Esc keys. Virtual for
    // controllers whose panel isn't scaffold-shaped (Move Face anchors its
    // value wells to the viewport window and renders them from there).
    virtual void renderPanel(const IopContext& ctx);

    bool active() const { return m_active; }
    bool previewOk() const { return m_previewOk; }

    // ─── Viewport handles (optional) ─────────────────────────────────────────
    // Ops with an on-screen handle own their hit-test and drag instead of
    // Application_Viewport doing it for them. Default: no handle, so the five
    // panel-only controllers are unaffected.
    virtual bool wantsViewportInput() const { return false; }
    // One frame of pointer input while this op is active and the camera isn't
    // being dragged. Call setDraggingHandle() when a handle latches so the
    // camera and the picker know to stand off.
    virtual void onViewportInput(const IopViewport& vp, const IopContext& ctx) {
        (void)vp; (void)ctx;
    }
    // Draw the handles. Called with the foreground draw list already selected.
    virtual void drawOverlay(const IopOverlay& ov) const { (void)ov; }
    // Draw the op's real 3D gizmo meshes. Unlike drawOverlay this runs during
    // the 3D pass with the viewport FBO still bound — see IopGizmo3D for why
    // the two phases must not be mixed up.
    virtual void drawGizmos3D(const IopGizmo3D& g) const { (void)g; }

    // True while a handle is latched. The viewport suppresses camera orbit and
    // face picking on this — previously read off ScaleFace's dragAxis()
    // directly, which only worked because exactly one controller had a gizmo.
    bool draggingHandle() const { return m_draggingHandle; }

protected:
    virtual const char* title() const = 0;
    // Which preview engine this op runs on — see the two models above.
    virtual PreviewModel previewModel() const { return PreviewModel::SnapshotBody; }
    // Capture the selection into params. Return the target body id, -1 to
    // refuse to start (nothing usable selected), or kNoTargetBody for a
    // LiveOp controller that creates its own body.
    virtual int onBegin(const IopContext& ctx) = 0;
    // Build a fresh Operation from the current parameters. SnapshotBody calls
    // this for every preview AND the commit; LiveOp calls it ONCE per gesture
    // to mint the instance it then keeps (which is what makes created-body ids
    // stable), and feeds later value changes through syncLiveOp instead.
    virtual std::unique_ptr<Operation> buildOp(const IopContext& ctx) = 0;
    // LiveOp only: push the current parameter values into the live instance.
    // Return false when the gesture is currently a no-op (zero distance), so
    // the base skips execute() and leaves the document un-previewed.
    virtual bool syncLiveOp(Operation& op) { (void)op; return true; }
    // LiveOp only: the op to RECORD at commit when it differs from the one
    // previewed — Extrude previews a NewBody tool volume but commits a real
    // boolean cut against the body the sketch was drawn on. Returning null
    // (the default) records the live instance as-is via pushExecuted.
    virtual std::unique_ptr<Operation> buildCommitOp(const IopContext& ctx) {
        (void)ctx; return nullptr;
    }
    // Parameter widgets (sliders, radios, status lines). Set `changed`
    // when a value moved so the scaffold re-runs the preview. Call
    // requestCommit() to commit from inside the body (e.g. Enter in a
    // text field).
    virtual void panelBody(const IopContext& ctx, bool& changed) = 0;
    virtual void onCleanup() {}
    virtual float panelWidth() const { return 260.0f; }
    // How a finished preview frame invalidates meshes. Default: mark
    // EVERYTHING, which triggers a full clear + re-tessellate of every visible
    // body. That is correct for an op that edits one known body, and wrong two
    // ways for Push/Pull — it re-tessellates 100+ untouched bodies per drag
    // frame, and the clear() wipes renderer-only slots (the ghost tool volume
    // has no Document body behind it, so nothing puts it back).
    virtual void markPreviewDirty(const IopContext& ctx) const {
        ctx.markMeshesDirty();
    }
    // Override to suppress the per-change live preview when recomputing it would
    // freeze the UI (e.g. projecting a sketch with hundreds of regions). Commit
    // still builds + runs the op once. Default: always preview.
    virtual bool wantsLivePreview(const IopContext&) const { return true; }
    // Should commit() hand the op to deferHeavy (runs BETWEEN frames behind a
    // cancellable progress window) instead of pushing it inline?
    //
    // Defaults to "whenever the live preview is off", which is right for an op
    // that skipped previewing BECAUSE it is slow (Project Sketch). It is wrong
    // for one that skipped previewing for a different reason: Resize
    // Cylindrical turns the preview off on a THREADED body — the ring boolean
    // would re-run against the thread's helicoid faces on every keystroke —
    // but its commit is cheap and must stay inline, because History reflows it
    // beneath the Thread step and re-cuts the thread around that push.
    virtual bool wantsDeferredCommit(const IopContext& ctx) const {
        return !wantsLivePreview(ctx);
    }

    void requestCommit() { m_commitRequested = true; }
    void setDraggingHandle(bool d) { m_draggingHandle = d; }
    // For a controller that runs its own preview (HistoryEdit) and has to
    // report whether the frame landed.
    void setPreviewOk(bool ok) { m_previewOk = ok; }
    // HistoryEdit: the "pre-state" shape isn't the current body — it is the
    // edited op's own getPreviousShape(). Set it from onBegin.
    void setSnapshot(const TopoDS_Shape& s) { m_snapshot = s; }
    // Tear the controller down WITHOUT touching the document — for a
    // controller whose commit/cancel already put the model where it belongs.
    void teardown() { cleanup(); }
    // For custom-lifecycle controllers only: keeps the base active() flag —
    // what every generic loop gates on — in step with their own state.
    //
    // Deactivating also drops a latched handle, for the same reason cleanup()
    // does: a controller that is not active cannot own a viewport drag, and a
    // latch left set makes anyIopDraggingHandle() true forever, which suppresses
    // camera orbit AND pan for the rest of the session. The ViewCube keeps
    // working — it doesn't go through the drag path — so it reads as "the mouse
    // broke" rather than "an op never finished", which is what made it hard to
    // place. Move Face is the controller that hits this: it commits and cancels
    // through setActive(false) rather than cleanup(), so it was the one custom
    // lifecycle that skipped the line cleanup() has for exactly this hazard.
    // Reached by cancelling or committing mid-gesture, and by opening another
    // project — which cancels every active controller.
    void setActive(bool a) { m_active = a; if (!a) m_draggingHandle = false; }

    int bodyId() const { return m_bodyId; }
    const TopoDS_Shape& snapshot() const { return m_snapshot; }
    // LiveOp: the instance currently driving the preview (null before the
    // first update, or after a commit/cancel released it).
    Operation* liveOp() const { return m_liveOp.get(); }
    // LiveOp: is the live instance currently APPLIED to the document?
    bool livePreviewApplied() const { return m_liveApplied; }

private:
    void cleanup();
    void updateLive(const IopContext& ctx);

    bool m_active = false;
    bool m_previewOk = false;
    bool m_commitRequested = false;
    bool m_draggingHandle = false;
    int m_bodyId = -1;
    TopoDS_Shape m_snapshot;
    // LiveOp model only — see PreviewModel.
    std::unique_ptr<Operation> m_liveOp;
    bool m_liveApplied = false;
};

} // namespace materializr
