#pragma once
#include "../core/Operation.h"
#include "../core/Document.h"
#include "Sketch.h"
#include <gp_Pln.hxx>
#include <memory>
#include <map>
#include <set>
#include <vector>

namespace materializr {

// Snapshot-based undo for sketch mutations (drawing elements, dimension input).
// Holds a shared_ptr to the live Sketch (same instance the Document and the
// active editing session share) plus before/after snapshots. Undo/redo swap
// the live sketch's contents in place, so the change is visible regardless of
// whether the sketch is in the document yet (a fresh sketch isn't added until
// exitSketchMode).
class SketchEditOp : public Operation {
public:
    SketchEditOp(std::shared_ptr<Sketch> liveSketch,
                 std::shared_ptr<Sketch> beforeSnapshot,
                 std::shared_ptr<Sketch> afterSnapshot);

    bool execute(Document& doc) override;
    bool undo(Document& doc) override;
    std::string name() const override { return "Sketch Edit"; }
    std::string description() const override;
    // Lists every dimension constraint (Distance / Radius / Angle) on the
    // post-edit snapshot with an editable value. Mutating a value here updates
    // m_after AND re-solves the snapshot so dependent geometry shifts to match
    // before the user clicks Apply Changes. Non-dimensional constraints
    // (Horizontal, Parallel, etc.) are shown as read-only entries because
    // they have nothing user-tunable.
    void renderProperties() override;
    std::string typeId() const override { return "sketchedit"; }

    // Cross-session serialization: writes both snapshots in the project's
    // SKETCH_START/SKETCH_END text format so a reloaded op can rehydrate
    // into a real SketchEditOp (not a parameterless ReplayOp) and the
    // History → Properties editor still works. The Document& lets us look
    // up the live sketch's id from the held shared_ptr at write time;
    // deserialize is a free function in ProjectIO since it needs to
    // bind m_target to the loaded sketch by id.
    std::string serializeWithDocument(const Document& doc) const;

    // Elements this step introduced (present in the after-snapshot but not the
    // before-snapshot) - i.e. the line(s)/circle(s)/arc(s) the step draws or
    // resizes. Used to highlight "what's being edited" in the viewport when the
    // step is selected in the history panel. Falls back to nothing if there's
    // no before-snapshot.
    void getEditedElements(std::set<int>& lines, std::set<int>& circles,
                           std::set<int>& arcs) const;

    // True when the step changed only the cosmetic fill anchors (a Fill-tool
    // click): same geometry, same constraints. Nothing a body is built from
    // moved, so undo/redo must not cascade it into a body rebuild.
    bool onlyFillsChanged() const;

    // Accessors for the ProjectIO factory.
    std::shared_ptr<Sketch> getBeforeSnapshot() const { return m_before; }
    std::shared_ptr<Sketch> getAfterSnapshot() const  { return m_after;  }
    std::shared_ptr<Sketch> getTarget() const { return m_target; }
    void setTarget(std::shared_ptr<Sketch> t) { m_target = std::move(t); }
    // Remember which sketch this op edits, captured at creation time when the
    // target is definitely in the document. serializeWithDocument uses it if
    // the live-pointer lookup fails at save time (a stale/replaced m_target
    // otherwise silently produced an EMPTY params blob, freezing the step on
    // reload - the "Remove sketch element" warning).
    void setSketchId(int id) { m_sketchId = id; }
    int  getSketchId() const { return m_sketchId; }
    void setSnapshots(std::shared_ptr<Sketch> before, std::shared_ptr<Sketch> after) {
        m_before = std::move(before); m_after = std::move(after);
    }

    // Sketches drawn on a body this sketch drives, carried along when the edit
    // slid or turned the WHOLE sketch (every point moved rigidly - the sketch
    // was dragged as one piece). The body rebuilds in the new place, so a sketch
    // sitting on it has to go with it or its features (cuts, push/pulls) miss
    // and get switched off (issue #185). Poses are absolute, so a replay lands
    // them in the same place every time; undo puts them back with the edit.
    struct FollowerMove { int id; gp_Pln before; gp_Pln after; };
    void addFollowerMove(int id, const gp_Pln& before, const gp_Pln& after) {
        m_followerMoves.push_back({id, before, after});
    }
    const std::vector<FollowerMove>& followerMoves() const { return m_followerMoves; }
    // True once the app has looked at this edit for follower motion (done once,
    // right after the edit is recorded). A reloaded step counts as looked at:
    // its followers, if any, came back with it.
    bool followersAnalyzed() const { return m_followersAnalyzed; }
    void setFollowersAnalyzed(bool v) { m_followersAnalyzed = v; }

    // Inline diameter edits made in renderProperties (circleId -> new radius).
    // Each sketchedit step holds a FULL snapshot of the sketch, so a radius
    // edited here is silently overwritten by the next step's snapshot on replay
    // before any extrude/pushpull reads it. The Apply path uses this map to push
    // the new radius into every later snapshot of the same sketch.
    const std::map<int, double>& editedCircleRadii() const { return m_editedCircleRadii; }
    void clearEditedCircleRadii() { m_editedCircleRadii.clear(); }
    // Apply a diameter edit to THIS step's after-snapshot and record it for
    // forward propagation. Single entry point used by renderProperties and tests.
    void editCircleRadius(int circleId, double radius) {
        if (m_after) m_after->setCircleRadius(circleId, radius);
        m_editedCircleRadii[circleId] = radius;
    }
    // Sets the circle's radius in BOTH snapshots if the circle is present
    // (no-op otherwise). Used to carry an upstream diameter edit forward.
    void applyCircleRadiusToSnapshots(int circleId, double radius);

private:
    std::shared_ptr<Sketch> m_target;
    int m_sketchId = -1;   // cached document id (see setSketchId)
    std::shared_ptr<Sketch> m_before;
    std::shared_ptr<Sketch> m_after;
    std::map<int, double> m_editedCircleRadii;
    std::vector<FollowerMove> m_followerMoves;
    bool m_followersAnalyzed = false;
};

} // namespace materializr
