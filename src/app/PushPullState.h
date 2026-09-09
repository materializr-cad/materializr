#pragma once
#include <TopoDS_Face.hxx>
#include <glm/glm.hpp>
#include <vector>

namespace materializr {

// The interactive Push/Pull gesture's parameters, owned by PushPullController.
//
// Was 16 loose members on Application; slice 1 grouped them here, slice 2 moved
// the lifecycle onto InteractiveOpController's LiveOp preview model - which is
// the engine this op was hand-written with in the first place. Five of the
// original members went with that move rather than coming along: `active`,
// `liveOp` and `previewApplied` are the base's now, and `previewBodyIds` /
// `previousBodies` turned out to be dead (cleared at begin, never read).
struct PushPullState {
    // One entry per region/face the gesture will operate on.
    struct Target {
        int sketchId;
        int regionIndex;
        int sourceBodyId;   // -1 for floating (NewBody)
        TopoDS_Face profile;
    };

    bool symmetric = false;   // panel checkbox (plane-sketch targets)
    float distance = 5.0f;
    // Unsnapped drag accumulator. The grid snap in updatePushPull mutates
    // `distance` itself (so the readouts show the snapped value), which would
    // erase sub-step drag motion every frame - a slow drag accumulated
    // nothing, then a fast flick jumped a whole step. The drag adds into THIS
    // instead, and `distance` is derived + snapped from it. Typing/sliding a
    // value re-bases the accumulator.
    float distanceRaw = 0.0f;
    char inputBuf[32] = "5.0";
    bool inputFocus = true;

    // Face arrow: drag along this normal to drive the distance (set from the
    // first face target). `hasArrow` is false for sketch-region-only push/pull.
    glm::vec3 origin{0.0f};
    glm::vec3 normal{0.0f, 0.0f, 1.0f};
    bool hasArrow = false;

    // Trackpad-mode sticky drag (orbitButton == panButton == LMB): a single
    // click in the viewport while the arrow is up enters this state, mouse
    // moves then drive the distance frame-by-frame without a button held,
    // and a second click exits. Same shape as the Sketch Circle tool's
    // click-move-click pattern - gives users a way to "drag" the arrow
    // when their primary click is already bound to orbit. While true,
    // gizmoOwnsDrag suppresses orbit so the cursor isn't fighting the
    // camera. (Steve: "let click then click act like click and hold".)
    bool sticky = false;
    bool pressWasDrag = false;   // press became a drag: not a sticky toggle

    // Dense-body drag protection: when any target body has >250 faces (a
    // threaded rod), the per-frame preview shows a tinted GHOST of the tool
    // volume instead of running the real boolean (which would also trigger
    // the thread reflow) every frame. The real op runs once, on commit.
    bool heavyPreview = false;
    // Any VISIBLE body carries a thread. Wider than the target list on
    // purpose: a cut-intersecting push/pull booleans into everything in the
    // tool's path. Keeps the commit inline (see
    // PushPullController::wantsDeferredCommit).
    bool threadedPath = false;

    std::vector<Target> targets;
};

} // namespace materializr
