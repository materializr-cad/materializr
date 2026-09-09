#pragma once
#include <TopoDS_Shape.hxx>
#include <map>

class Document;
class History;

namespace materializr {

// The THIRD preview model: live-editing a step that is ALREADY on History.
//
// Neither of the others fits an existing fillet whose radius is being dragged.
// PreviewModel::SnapshotBody restores one body and runs a transient op - but a
// committed fillet has downstream steps (a chamfer stacked on it, a cut through
// it) that must recompute too, or they flicker out for the length of the drag.
// PreviewModel::LiveOp toggles an UNRECORDED instance - but this op is already
// recorded, and its parameter is the very thing being dragged.
//
// So the preview IS the real replay: mutate the step's parameter in place and
// call History::editStep(). That buys correct downstream geometry and costs two
// things, which are exactly what this class exists to guard.
//
//   1. A replay can fail PARTWAY. A fillet whose edges reference geometry that
//      a later feature consumed loads fine but cannot re-execute; editStep then
//      leaves the model half-rebuilt, with stray planar faces where the blend
//      used to be. begin() snapshots every body so replay() can put it all back.
//   2. The preview frames run editStep NON-transactionally, so every op
//      re-resolves its edges and refs against the PREVIEW bodies. If the
//      snapshot is later restored without also restoring that resolution state,
//      the step wedges - silently failing on every subsequent edit until the
//      project is reloaded. Hence History::snapshotAllEditState() alongside the
//      body snapshot, and restoreAllEditState() alongside the body restore.
//
// After restoring, History::markFullyApplied() tells history the model matches
// its steps again: nothing re-executed, so undo/redo must not think otherwise.
class HistoryEditPreview {
public:
    // Capture every body and every op's edit state. Call BEFORE the first
    // preview replay.
    void begin(Document& doc, History& hist);

    bool active() const { return m_active; }

    // Replay the edited step. On failure the snapshot is restored for you and
    // this returns false - so a caller that gets `false` knows the model is
    // back at its pre-edit state, not stranded mid-replay.
    bool replay(int stepIndex, Document& doc, History& hist);

    // Put every body back, drop any body a failed replay spawned, restore the
    // ops' edit state, and mark history fully applied. False if no snapshot.
    bool restore(Document& doc, History& hist);

    void clear();

private:
    std::map<int, TopoDS_Shape> m_bodies;
    bool m_active = false;
};

} // namespace materializr
