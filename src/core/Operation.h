#pragma once
#include <chrono>
#include <string>
#include <memory>
#include <vector>
#include <utility>
#include <functional>
#include <TopoDS_Shape.hxx>

class Document; // forward declare

// A non-destructive description of the body changes an operation made, read
// straight from the op's stored undo data. Used to serialize history without
// calling undo()/execute() (which mutate op state and recompute geometry).
struct OperationDiff {
    std::vector<std::pair<int, TopoDS_Shape>> modifiedBefore; // body id -> shape BEFORE this op
    std::vector<int> created;                                 // ids this op created
    std::vector<std::pair<int, TopoDS_Shape>> deletedBefore;  // id -> shape this op deleted
};

class Operation {
public:
    virtual ~Operation() = default;

    virtual bool execute(Document& doc) = 0;
    virtual bool undo(Document& doc) = 0;
    virtual std::string name() const = 0;
    virtual std::string description() const = 0;

    // For the properties panel - each operation renders its own ImGui editor
    virtual void renderProperties() = 0;

    // Unique type identifier for SERIALIZATION. These strings are the on-disk
    // format keys - ProjectIO writes `TYPE <typeId>` and OperationFactory
    // rebuilds from them - so a value here can never change without breaking
    // every saved project. Use kind() to make DECISIONS about an op; see below.
    virtual std::string typeId() const = 0;

    // The same identity, typed, for behaviour that branches on op type.
    //
    // Dispatch used to compare typeId() against string literals scattered
    // across History, Application and the Toolbar - "thread", "shell",
    // "moveface", "fillet", "chamfer". Every one of those is a silent failure
    // waiting to happen: misspell it and the compare simply never matches, so
    // a thread quietly stops reflowing (or, as in isBodyThreaded, keeps
    // reflowing when it shouldn't) with nothing to point at. This is the same
    // brittleness derei flagged for the plugin dispatcher in #72, which became
    // the typed InteractiveOp enum in 543f42b; typeId() was the other half.
    //
    // Deliberately NOT a virtual each op overrides: it is derived from
    // typeId() by the single mapping at the bottom of this header, so the
    // string↔kind correspondence lives in exactly one place and no op can
    // drift out of sync with it. An op whose typeId isn't listed gets Other,
    // which is the right answer for the ~30 types nothing branches on.
    enum class Kind { Other, Thread, Shell, MoveFace, Fillet, Chamfer };
    Kind kind() const;
    bool isKind(Kind k) const { return kind() == k; }

    // True if this operation produced the given face (e.g. a fillet/chamfer
    // blend surface). Lets the UI map a clicked face back to the op that made
    // it so the user can re-edit it. Default: operations own no faces.
    virtual bool ownsFace(const TopoDS_Shape& /*face*/) const { return false; }

    // Match QUALITY for the Edit Fillet/Chamfer picker: 0 = not owned,
    // 2 = exact (the face IsSame one of this op's generated faces on the live
    // body), 1 = a post-rebuild geometric-centre fallback match. The picker
    // prefers the highest score so a fuzzy over-match from an unrelated op
    // (e.g. a big multi-edge fillet whose blend sits near a later countersink
    // chamfer) can't steal a face its true owner claims exactly - the old
    // first-op-in-history-wins loop mis-attributed exactly that case (#49).
    // Default mirrors ownsFace() so non-fillet/chamfer ops need no override.
    virtual int ownsFaceScore(const TopoDS_Shape& face) const {
        return ownsFace(face) ? 1 : 0;
    }

    // Report the body changes this op made, read from its stored undo data.
    // Non-destructive (unlike undo()). Default: no body changes (e.g. a sketch
    // edit). Used to persist the operation history in the project file.
    virtual OperationDiff captureDiff() const { return {}; }

    // True for a step reconstructed from a saved project: it replays stored
    // geometry for undo/redo but its parameters can't be re-edited. Lets the UI
    // flag such steps after a project is reopened.
    virtual bool isReloaded() const { return false; }

    // True only for a reloaded step the user should be WARNED about: one that
    // lost its editable parameters AND shapes body geometry (a baked fillet,
    // boolean, etc.). A sketch-only step that reloaded without params is inert
    // - the sketch itself loads fine and there is nothing to "repair" - so it
    // is deliberately NOT a frozen feature and must not raise the amber banner.
    virtual bool isFrozenFeature() const { return false; }

    // Serialise this op's input parameters (radii, distances, axis, etc.) as
    // a single-line opaque text blob. Empty default = nothing to save (sketch
    // edits, replay ops, simple ops without parameters). Read back by
    // deserializeParams; returns true on a clean parse. The format is up to
    // each op - keep it stable per typeId or version it inside the blob.
    virtual std::string serializeParams() const { return ""; }
    virtual bool deserializeParams(const std::string& /*blob*/) { return true; }

    // The body changes a step made, reconstructed from the saved project on
    // load. Handed to rehydrateFromReload() so a freshly-deserialized op can
    // restore the bookkeeping it would normally have built during execute()
    // (which ids it created / modified / deleted), without re-running the
    // geometry. `created` ids are exactly the bodies this step introduced;
    // `modifiedBefore` / `deletedBefore` carry the prior shape so undo() can
    // restore them.
    struct ReloadState {
        std::vector<int> created;
        // The created bodies' shapes AFTER this step - lets a sketch-driven
        // extrude derive WHICH regions it originally used from its own saved
        // result's footprint on the sketch plane (#53, old-file recovery).
        std::vector<std::pair<int, TopoDS_Shape>> createdAfter;
        std::vector<std::pair<int, TopoDS_Shape>> modifiedBefore;
        // The same modified bodies AFTER this step - sub-shape-referencing ops
        // resolve their generated-geometry indices (e.g. fillet blend faces
        // for click-to-edit) against the result shape.
        std::vector<std::pair<int, TopoDS_Shape>> modifiedAfter;
        std::vector<std::pair<int, TopoDS_Shape>> deletedBefore;
    };

    // Restore an op (already populated via deserializeParams) to its
    // post-execution state from a saved project, so undo()/redo() and
    // parameter re-editing work across sessions. `doc` is the live document
    // (sketch-sourced ops re-derive their profile from a persistent sketch id
    // through it). Return true if this op is now a fully editable reloaded op;
    // the default returns false, which tells the loader to fall back to a baked
    // ReplayOp (preserving prior behaviour for every op that hasn't opted in).
    // Ops that reference whole bodies by id (patterns) or a persistent sketch
    // (extrude/revolve) can implement this; ops that reference specific raw
    // sub-shapes (fillet edges, face push/pull) need persistent topological
    // naming first and should leave it unimplemented.
    virtual bool rehydrateFromReload(const ReloadState& /*state*/, Document& /*doc*/) { return false; }

    bool isEnabled() const { return m_enabled; }
    void setEnabled(bool enabled) { m_enabled = enabled; }

    // Clone this operation retargeted at another body. Used by the
    // thread-last reflow to propagate a finishing pass onto bodies the
    // inserted op created (the second half of a split gets its own thread
    // cut). Default: not cloneable.
    virtual std::unique_ptr<Operation> cloneForBody(int /*bodyId*/) const {
        return nullptr;
    }

    // Body ids this op will read/modify when executed - known BEFORE
    // execution for ops that carry explicit target ids. Used by the
    // thread-last reflow: an op that touches a threaded body is inserted
    // BEFORE the trailing Thread steps so its boolean runs against clean
    // geometry (OCCT can't classify cuts along helical groove fields), and
    // the thread re-cuts parametrically afterwards. Empty (the default)
    // means "unknown / not boolean-sensitive" - no reflow.
    virtual std::vector<int> plannedBodyIds() const { return {}; }

    // The sub-shapes this op was handed as parameters (the faces to remove,
    // the edges to fillet) while it still holds them as live references, so
    // an off-thread preview can point them at a private copy of the body
    // before execute() runs there (SnapshotPreview). An op that returns
    // nothing while it does hold such references is not eligible for that
    // preview and keeps running in the frame. Default: no shape parameters.
    virtual std::vector<TopoDS_Shape*> shapeParams() { return {}; }

    // Maintained by History: the serialised parameter set from this op's last
    // SUCCESSFUL execute. Used to roll a rejected edit back - the UI mutates
    // params in place before editStep runs, so "the values that worked" must
    // be captured at execute time, not edit time. Empty for ops without
    // parameter serialisation (no rescue possible - they fail-and-suspend).
    const std::string& lastGoodParams() const { return m_lastGoodParams; }
    void rememberGoodParams() { m_lastGoodParams = serializeParams(); }

    // Transactional edit-state rollback (History::editStep). A doomed replay
    // executes ops that individually SUCCEED before a later step fails; those
    // executes mutate op-internal resolution state (a fillet/chamfer rewrites
    // its stored edges/anchors/refs against the mid-replay bodies). Restoring
    // the BODY snapshot alone leaves that state pointing at geometry that no
    // longer exists, so the next edit attempt fails where a fresh session
    // succeeds (the "one failed edit wedges the step until reload" bug).
    // History snapshots every op before a transactional replay and restores
    // them on failure. Default: stateless (most ops re-derive everything from
    // the document); ops holding resolved sub-shape state override both.
    virtual void snapshotEditState() {}
    virtual void restoreEditState() {}

    // Wall-clock time this op was constructed (or restored to a stored value
    // on project load). Used by the HistoryPanel to bucket steps into
    // "Today / Yesterday / <date>" collapsible groups so a 145-step project
    // is browsable without endless scrolling.
    std::chrono::system_clock::time_point timestamp() const { return m_timestamp; }
    void setTimestamp(std::chrono::system_clock::time_point t) { m_timestamp = t; }

    // Optional progress sink for long operations (thread cutting, dense
    // projection). The app sets it before execute(); the op calls
    // reportProgress() at natural milestones (per turn / per region). The sink
    // renders a progress frame and pumps the event loop, so the window stays
    // alive on slow machines. Returns true if the user asked to cancel.
    void setProgressReporter(std::function<bool(float, const char*)> r) {
        m_progress = std::move(r);
    }

protected:
    // Call from inside a long execute() loop. Returns true if the user
    // cancelled, so the op can bail and report failure. No-op (returns false)
    // when no reporter is set - ops work exactly as before.
    bool reportProgress(float fraction, const char* label) {
        return m_progress ? m_progress(fraction, label) : false;
    }

    bool m_enabled = true;
    std::string m_lastGoodParams; // see lastGoodParams()
    std::chrono::system_clock::time_point m_timestamp = std::chrono::system_clock::now();
    std::function<bool(float, const char*)> m_progress;
};

// The one place a type-id string is turned into a decision. Keep these values
// EXACTLY as each op's typeId() returns them - they are the saved-file keys
// (see typeId() above). Adding a Kind means adding it here and nowhere else.
inline Operation::Kind Operation::kind() const {
    const std::string id = typeId();
    if (id == "thread")   return Kind::Thread;
    if (id == "shell")    return Kind::Shell;
    if (id == "moveface") return Kind::MoveFace;
    if (id == "fillet")   return Kind::Fillet;
    if (id == "chamfer")  return Kind::Chamfer;
    return Kind::Other;
}
