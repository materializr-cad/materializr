#pragma once
#include <memory>

class Document;
class History;
class Operation;

namespace materializr {

// ONE operation instance, toggled against the document to drive a live
// preview. History sees nothing until commit().
//
// This is the engine InteractiveOpController calls PreviewModel::LiveOp, in a
// form an op that is NOT a controller can use. It exists because five
// interactive ops - Pattern, Loft, Boundary Fill, Construction Plane,
// Construction Axis - all previewed the other way:
//
//     if (m_fooPreviewPushed && m_history->canUndo()) m_history->undo(doc);
//     ...
//     if (m_history->pushOperation(std::move(op), doc)) m_fooPreviewPushed = true;
//
// i.e. a REAL history step pushed and undone on every preview frame. Three
// things are wrong with that, all of which this class removes:
//
//   1. `canUndo()` is not "my preview is on top". It undoes whatever the top
//      step happens to be. Anything that touches history mid-gesture - a
//      cascade re-execute, a thread recut landing, another preview - and the
//      next parameter change silently undoes the USER's work instead.
//      Extrude used to carry an explicit "preview op no longer on top of
//      history - resyncing without undo" bail-out for exactly this; these
//      five never had one.
//   2. A fresh op instance per frame throws away the id-reuse pool that
//      PatternOp/LoftOp keep across undo (m_reuseBodyIds / m_createdBodyId),
//      so every created body changes id on every frame. Downstream references
//      and the renderer's per-body slots both churn.
//   3. The preview is a real, visible, undoable history step while the popup
//      is still open - Ctrl+Z lands in the middle of the gesture, and the
//      History panel shows a step the user has not committed to.
//
// Keeping one instance and calling undo()/execute() on it directly fixes all
// three: ids stay stable because the same instance re-uses the ones it minted,
// and commit() hands the already-applied instance to History via
// pushExecuted() without re-running it.
class LiveOpPreview {
public:
    // The instance driving the preview (null before the first hold()).
    Operation* op() const { return m_op.get(); }
    // Is that instance currently APPLIED to the document?
    bool applied() const { return m_applied; }

    // Take ownership of a new instance, undoing and dropping any previous one.
    // Use when the op TYPE changes mid-gesture (Loft ⇄ Guided Loft) or on the
    // first preview; parameter changes should re-sync the held instance
    // instead, which is the whole point.
    void hold(std::unique_ptr<Operation> newOp, Document& doc);

    // Undo the applied preview so the caller can push new parameters into the
    // held instance. No-op when nothing is applied.
    void retract(Document& doc);

    // (Re-)execute the held instance. Returns whether it landed; a failure
    // leaves nothing applied, so the document keeps its un-previewed state.
    bool apply(Document& doc);

    // Undo and drop - the cancel path.
    void clear(Document& doc);

    // Record the applied instance WITHOUT re-running it (pushExecuted), and
    // release it. Returns false if nothing was applied, in which case history
    // is untouched, which is right: the document is already unmodified.
    bool commit(History& hist);

private:
    std::unique_ptr<Operation> m_op;
    bool m_applied = false;
};

} // namespace materializr
