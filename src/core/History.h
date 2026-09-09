#pragma once
#include <vector>
#include <memory>
#include "Operation.h"
#include "Document.h"

#include <functional>
namespace materializr { class EventBus; }

class History {
public:
    History();
    ~History() = default;

    void setEventBus(materializr::EventBus* bus) { m_eventBus = bus; }
    // Called when a pushOperation is declined because the target body has
    // a Thread step (threads-last discipline). The app shows a toast.
    void setThreadsLastDeclineCallback(std::function<void()> cb) {
        m_threadsLastDecline = std::move(cb);
    }

    // Add a new operation (executes it and pushes to stack)
    bool pushOperation(std::unique_ptr<Operation> op, Document& doc);

    // Add an operation whose effect is already applied to the document.
    // Used for ops where the live mutation happened externally (e.g. sketch
    // edits performed by the SketchTool); the op snapshots before/after so
    // undo/redo can swap between them without re-running the original action.
    void pushExecuted(std::unique_ptr<Operation> op);

    // Undo/Redo
    bool canUndo() const;
    bool canRedo() const;
    bool undo(Document& doc);
    bool redo(Document& doc);

    // Undo floor: forbid undo from crossing at or below this step index. Set
    // while a sketch is open so NO undo path (menu, History panel, Ctrl+Z, the
    // plugin command) can roll the document back past sketch entry into the host
    // body - undoing the body while the live sketch renders against it crashes
    // (SIGABRT, heap corruption). canUndo() honours it, so the Undo buttons also
    // grey out at the floor. -1 = no floor.
    // The step editStep() last hard-failed on (reset to -1 at each call) -
    // lets the sketch-edit cascade DISABLE the un-followable step, retry, and
    // tell the user WHICH feature needs re-applying instead of reverting the
    // whole edit with a guess of a message (#53).
    int lastEditFailStep() const { return m_lastEditFailStep; }
    void setUndoFloor(int step) { m_undoFloor = step; }
    void clearUndoFloor() { m_undoFloor = -1; }

    // History navigation
    int stepCount() const;
    int currentStep() const; // index of last executed step
    const Operation* getStep(int index) const;

    // Monotonic counter bumped by every mutating call (push/undo/redo/edit/
    // remove/replay/enable/clear/dropRedoTail). Lets callers memoize anything
    // derived from the op list - e.g. the sketch↔body link map, which used to
    // be rebuilt (full history walk + captureDiff per op) EVERY FRAME by the
    // Properties panel's link hint. A spurious bump only invalidates a cache;
    // a missed one would serve stale data, so mutators bump unconditionally,
    // even when the action ends up a no-op.
    unsigned revision() const { return m_revision; }

    // Thread-last reflow support: if `op` plans to touch a body that the
    // trailing Thread steps modified, returns the index those threads start
    // at (where the op should be inserted); -1 = no reflow needed.
    int reflowInsertionIndex(const Operation& op) const;
    // Insertion index for the SHELL auto-reflow: a face transform (moveface)
    // on a SHELLED body corrupts - the loft engine can't do cavities - but is
    // perfectly computable the other way round: apply it to the pre-shell
    // solid, then re-run the shell on the moved body ("the order flipped").
    // Unlike threads (multi-second recompute → user-discipline refusal), a
    // shell re-execute is sub-second, so it reflows automatically. -1 = no
    // shell touches the op's bodies / op isn't a face transform.
    int shellReflowIndex(const Operation& op) const;
    // True if a Thread step in the applied history modified this body.
    // Interactive ops (push/pull, resize, …) check this at BEGIN to refuse
    // up front - their per-frame preview would otherwise run a boolean
    // against the thread's thousands of faces every frame and freeze,
    // never reaching the commit-time refusal in pushOperation.
    bool isBodyThreaded(int bodyId) const;
    // True if a Shell step in the applied history modified this body - the
    // face-op UI uses it to explain that the drag preview stays put on a
    // hollow body (the transform applies pre-shell on release).
    bool isBodyShelled(int bodyId) const;
    // Insert `op` at `index`, executing it against the state rolled back to
    // just before `index`, then replay the displaced steps (the threads
    // re-cut parametrically on the new geometry). Returns false only if the
    // INSERTED op itself fails (history left as it was); a displaced step
    // failing to re-execute suspends with lastReplayFailure(), same as
    // editStep.
    bool insertStepAndReplay(int index, std::unique_ptr<Operation> op,
                             Document& doc);

    // Edit a historical step's parameters and replay. Editing a step ABOVE
    // the current index (e.g. one suspended by a failed recompute) rolls
    // forward to it instead of refusing; a successful edit also auto-retries
    // a failure-suspended tail.
    //
    // `transactional`: snapshot the whole model (bodies + sketches) before
    // replaying and, if any downstream op fails, restore it completely so the
    // edit either fully applies or leaves the model exactly as it was - never a
    // half-built state. Pass true from one-shot Apply-Changes paths; leave false
    // for per-frame previews (the snapshot copy isn't free).
    bool editStep(int index, Document& doc, bool transactional = false);

    // After an inline sketch-dimension edit at `editedStep` (e.g. a circle's
    // diameter typed in its history-step properties), carry the new value into
    // every LATER sketchedit snapshot of the SAME sketch. Each sketchedit step
    // is a full snapshot, so without this the next step's snapshot overwrites
    // the edit on replay before a downstream extrude/pushpull ever reads it.
    // Call BEFORE editStep. No-op when the step recorded no inline value edits.
    void propagateSketchValueEdits(int editedStep, Document& doc);

    // Index of the step that most recently failed to recompute during an
    // editStep replay / redo (its result vanished from the viewport and it
    // sits above the current index). -1 = none. The UI uses this to explain
    // what happened instead of leaving steps silently missing.
    int lastReplayFailure() const { return m_failedReplayAt; }
    // Mark a step as failed-to-recompute AFTER the replay returned - used by
    // the async thread re-cut when the worker's result lands null (the new
    // geometry can't take the thread). Shows the same explainer banner.
    void suspendStep(int idx) { m_failedReplayAt = idx; }

    // Remove a step entirely (delete that operation), rebuilding the model in
    // place. Returns false and leaves the model unchanged if removing the step
    // makes a later, dependent operation fail (a conflict).
    bool removeStep(int index, Document& doc);

    // Breakpoint: suppress all steps after this index
    void setBreakpoint(int index); // -1 = no breakpoint
    int getBreakpoint() const;

    // Replay: re-execute all enabled steps from scratch
    bool replayAll(Document& doc);

    // Enable/disable a single step, rebuilding the model IN PLACE (no
    // doc.clear()) so base/imported bodies that no operation recreates - e.g.
    // the starting box a lone push/pull modifies - are preserved. Returns false
    // if the rebuild left a dependent step unable to recompute (recorded via
    // lastReplayFailure()); the model is still valid, just partial. This is the
    // toggle the history/properties UI should call - NOT replayAll, whose
    // doc.clear() would delete non-operation base bodies.
    bool setStepEnabled(int index, bool enabled, Document& doc);

    // Mark every step applied and clear any replay-failure/suspend flags,
    // WITHOUT re-executing anything. The caller is responsible for having
    // restored the document bodies to the fully-applied (loaded) state by
    // other means - used when an interactive edit can't rebuild and we revert
    // to a saved body snapshot: those steps fail on execute() but were fine on
    // load (rehydrate ≠ replay), so there is no execute-based path back to the
    // clean state; this resets the bookkeeping to match the restored bodies.
    void markFullyApplied() {
        m_currentIndex = static_cast<int>(m_operations.size()) - 1;
        m_failedReplayAt = -1;
        m_lastEditFailStep = -1;
        ++m_revision;
    }

    // Session-level edit-state protection for the interactive edit flows,
    // which run editStep NON-transactionally per preview frame (too hot for
    // per-frame snapshots). Snapshot every op's edit state once when the
    // interactive session BEGINS, restore it when the session reverts to its
    // body snapshot - otherwise ops that executed during doomed preview
    // replays keep resolution state pointing at discarded geometry, and the
    // step is wedged until reload (fails where a fresh session succeeds).
    void snapshotAllEditState() {
        for (auto& op : m_operations) op->snapshotEditState();
    }
    void restoreAllEditState() {
        for (auto& op : m_operations) op->restoreEditState();
    }

    // Clear history
    void clear();

    // Drop any steps past the current index (the redo tail). Used after load so a
    // reopened project never starts below the tip with a phantom redo stack
    // (which would, e.g., block autosave). No-op when already at the tip.
    void dropRedoTail();

    // Access operations for UI
    const std::vector<std::unique_ptr<Operation>>& operations() const;

private:
    std::vector<std::unique_ptr<Operation>> m_operations;
    int m_currentIndex = -1;
    int m_breakpoint = -1;
    int m_undoFloor = -1;   // see setUndoFloor(): floor for in-sketch undo
    int m_lastEditFailStep = -1; // see lastEditFailStep()
    unsigned m_revision = 0; // see revision()
    // Step that failed to recompute during the last editStep/redo replay;
    // cleared by manual undo, by a successful retry, or by clear().
    int m_failedReplayAt = -1;
    materializr::EventBus* m_eventBus = nullptr;
    std::function<void()> m_threadsLastDecline;
};
