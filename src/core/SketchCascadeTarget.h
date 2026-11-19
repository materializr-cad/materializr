#pragma once
#include "Operation.h"
#include "Document.h"
#include "History.h"
#include "../modeling/ExtrudeOp.h"
#include "../modeling/GuidedLoftOp.h"
#include "../modeling/LoftOp.h"
#include "../modeling/ProjectSketchOp.h"
#include "../modeling/PushPullOp.h"
#include "../modeling/RevolveOp.h"
#include "../modeling/SketchEditOp.h"
#include "../modeling/SweepOp.h"
#include "../modeling/SketchTransformOp.h"
#include <functional>
#include <map>
#include <set>
#include <vector>

namespace materializr {

// The sketch id `op`'s undo()/redo() drove via the sketch-edit cascade (a
// SketchEditOp/SketchTransformOp's own undo/redo only reverts sketch geometry;
// the body it drives is updated separately via SketchEditedEvent). -1 if `op`
// is null or isn't a sketch-mutating step.
inline int sketchIdForCascade(const Operation* op, const Document& doc) {
    if (!op) return -1;
    if (auto* se = dynamic_cast<const SketchEditOp*>(op)) {
        auto target = se->getTarget();
        return target ? doc.findSketchId(target.get()) : -1;
    }
    if (auto* st = dynamic_cast<const SketchTransformOp*>(op))
        return st->getSketchId();
    return -1;
}

// How far back Ctrl+Z may reach while `sketchId` is open for editing.
//
// A freshly opened sketch stops undo at the step it was opened on: past that
// lies the host body, and rolling it back under a live sketch crashed. But
// reopening a sketch you already drew in is the same editing session as far as
// the user is concerned - the lines from the last visit have to undo too,
// exactly as they do from the 3D view. So the floor drops below the run of
// this sketch's own edits that ends at the current step (nothing in between
// touches a body, so it is as safe as undoing within one visit), and stops at
// the first step that is anything else.
//
// Returns the step index undo must NOT go below (-1 = the very start).
inline int sketchUndoFloor(const History& history, const Document& doc, int sketchId) {
    int floor = history.currentStep();
    if (sketchId < 0) return floor;
    while (floor >= 0) {
        const Operation* op = history.getStep(floor);
        if (!op || !op->isEnabled() || sketchIdForCascade(op, doc) != sketchId) break;
        --floor;
    }
    return floor;
}

// Undo can leave a sketch empty: stepping back past the step that drew it
// restores the sketch to zero elements, but the sketch itself stays in the
// document. Left visible it is a blank entry that can be opened and Finished -
// which deletes it, so the redo that should bring its geometry back has
// nothing to land on. So a sketch emptied that way becomes a GHOST: hidden
// from every listing, kept in the document, and unghosted by the redo that
// refills it. Once the redo chain that would restore it is gone (a new step
// replaced the redo stack) the ghost is removed for real.
//
// `keepSketchId` (the sketch being edited right now) is never touched.
// Returns true when anything changed. Cheap enough to run whenever
// History::revision() moves.
inline bool syncGhostSketches(const History& history, Document& doc,
                              int keepSketchId = -1) {
    const int applied = history.currentStep();
    std::set<int> redoRefs;
    for (int i = applied + 1; i < history.stepCount(); ++i)
        if (int sid = sketchIdForCascade(history.getStep(i), doc); sid >= 0)
            redoRefs.insert(sid);
    bool changed = false;
    for (int sid : doc.getAllSketchIdsIncludingGhosts()) {
        if (sid == keepSketchId) continue;
        auto sk = doc.getSketch(sid);
        if (!sk) continue;
        const bool empty = sk->elementCount() == 0;
        if (doc.isSketchGhost(sid)) {
            if (!empty) {
                doc.setSketchGhost(sid, false);
                changed = true;
            } else if (!redoRefs.count(sid)) {
                doc.removeSketch(sid);
                changed = true;
            }
        } else if (empty && redoRefs.count(sid)) {
            doc.setSketchGhost(sid, true);
            changed = true;
        }
    }
    return changed;
}

// Re-derive the profile of every enabled sketch-sourced extrude / push-pull /
// lathe that references `sketchId` and return the index of the EARLIEST one that rebuilt
// (-1 if none). `*matchedOut` counts the ones that reference the sketch at all.
//
// Only steps that are currently APPLIED (index <= currentStep) are considered.
// The caller replays the chain from the returned index; a step above the tip is
// part of the redo stack, and handing one back made History::editStep take its
// "roll FORWARD" branch (index > tip) and re-execute everything in between -
// so undoing a Sketch Edit whose sketch drives a LATER op jumped the tip
// forward instead of back (the "stuck in an undo/redo loop" report). A step
// above the tip is rebuilt anyway when redo re-applies the sketch edit that
// feeds it, which runs this again.
inline bool opBuiltFromSketch(const Operation* op, int sketchId);   // below

inline int rederiveSketchDrivenOps(History& history, Document& doc, int sketchId,
                                   int* matchedOut = nullptr) {
    int earliest = -1, matched = 0;
    const int applied = history.currentStep();
    // A contour drawn after a feature was made is added to that feature. With
    // several features built from one sketch (an extrude of one loop, a push of
    // another) the MOST RECENT one takes it - it is what the user was last doing
    // with the sketch - and the rest leave it alone: every one of them taking it
    // would build it several times. (A lathe takes every region anyway, so when
    // it is the latest nothing else adopts.) That is for a feature applied to the
    // sketch AS A WHOLE; one applied to picked contours instead takes only copies
    // of those, whichever feature it is (SketchScope.h).
    int latestFeature = -1;
    for (int i = 0; i <= applied && i < history.stepCount(); ++i) {
        const Operation* op = history.getStep(i);
        if (op && op->isEnabled() && opBuiltFromSketch(op, sketchId)) latestFeature = i;
    }
    for (int i = 0; i <= applied && i < history.stepCount(); ++i) {
        Operation* op = const_cast<Operation*>(history.getStep(i));
        if (!op || !op->isEnabled()) continue;
        if (auto* ext = dynamic_cast<ExtrudeOp*>(op)) {
            if (ext->getSketchId() != sketchId) continue;
            ++matched;
            if (ext->rebuildProfileFromSketch(doc, true, i == latestFeature) && earliest < 0) earliest = i;
        } else if (auto* pp = dynamic_cast<PushPullOp*>(op)) {
            bool refs = false;
            const int tc = pp->targetCount();
            for (int t = 0; t < tc; ++t)
                if (pp->getSketchIdAt(t) == sketchId) { refs = true; break; }
            if (!refs) continue;
            ++matched;
            if (pp->rebuildProfileFromSketch(doc, sketchId, true, i == latestFeature) && earliest < 0)
                earliest = i;
        } else if (auto* rv = dynamic_cast<RevolveOp*>(op)) {
            if (rv->getSketchId() != sketchId) continue;
            ++matched;
            if (rv->rebuildProfileFromSketch(doc) && earliest < 0) earliest = i;
        } else if (auto* sw = dynamic_cast<SweepOp*>(op)) {
            if (sw->getProfileSketchId() != sketchId && sw->getPathSketchId() != sketchId) continue;
            ++matched;
            if (sw->rebuildFromSketches(doc) && earliest < 0) earliest = i;
        } else if (auto* lf = dynamic_cast<LoftOp*>(op)) {
            if (!lf->usesSketch(sketchId)) continue;
            ++matched;
            if (lf->rebuildFromSketches(doc) && earliest < 0) earliest = i;
        } else if (auto* gl = dynamic_cast<GuidedLoftOp*>(op)) {
            if (!gl->usesSketch(sketchId)) continue;
            ++matched;
            if (gl->rebuildFromSketches(doc) && earliest < 0) earliest = i;
        } else if (auto* pj = dynamic_cast<ProjectSketchOp*>(op)) {
            // Re-reads the sketch every time it executes, so there is no
            // profile to rebuild - it only needs the chain replayed from here.
            if (pj->getSketchId() != sketchId) continue;
            ++matched;
            if (earliest < 0) earliest = i;
        }
    }
    if (matchedOut) *matchedOut = matched;
    return earliest;
}

// True when `op` is an extrude / push-pull / lathe / sweep / loft / projection built
// from `sketchId`.
inline bool opBuiltFromSketch(const Operation* op, int sketchId) {
    if (auto* pj = dynamic_cast<const ProjectSketchOp*>(op))
        return pj->getSketchId() == sketchId;
    if (auto* lf = dynamic_cast<const LoftOp*>(op))
        return lf->usesSketch(sketchId);
    if (auto* gl = dynamic_cast<const GuidedLoftOp*>(op))
        return gl->usesSketch(sketchId);
    if (auto* ext = dynamic_cast<const ExtrudeOp*>(op))
        return ext->getSketchId() == sketchId;
    if (auto* rv = dynamic_cast<const RevolveOp*>(op))
        return rv->getSketchId() == sketchId;
    if (auto* sw = dynamic_cast<const SweepOp*>(op))
        return sw->getProfileSketchId() == sketchId || sw->getPathSketchId() == sketchId;
    if (auto* pp = dynamic_cast<const PushPullOp*>(op)) {
        for (int t = 0; t < pp->targetCount(); ++t)
            if (pp->getSketchIdAt(t) == sketchId) return true;
    }
    return false;
}

// The sketches a history step is built from (empty for steps that use none).
inline std::set<int> opSourceSketches(const Operation* op) {
    std::set<int> src;
    if (!op) return src;
    if (auto* ext = dynamic_cast<const ExtrudeOp*>(op)) {
        if (ext->getSketchId() >= 0) src.insert(ext->getSketchId());
    } else if (auto* pp = dynamic_cast<const PushPullOp*>(op)) {
        for (int t = 0; t < pp->targetCount(); ++t)
            if (pp->getSketchIdAt(t) >= 0) src.insert(pp->getSketchIdAt(t));
    } else if (auto* rv = dynamic_cast<const RevolveOp*>(op)) {
        if (rv->getSketchId() >= 0) src.insert(rv->getSketchId());
    } else if (auto* sw = dynamic_cast<const SweepOp*>(op)) {
        if (sw->getProfileSketchId() >= 0) src.insert(sw->getProfileSketchId());
        if (sw->getPathSketchId() >= 0) src.insert(sw->getPathSketchId());
    } else if (auto* pj = dynamic_cast<const ProjectSketchOp*>(op)) {
        if (pj->getSketchId() >= 0) src.insert(pj->getSketchId());
    } else if (auto* lf = dynamic_cast<const LoftOp*>(op)) {
        for (int k = 0; k < lf->profileCount(); ++k)
            if (lf->getProfileSketchId(k) >= 0) src.insert(lf->getProfileSketchId(k));
    } else if (auto* gl = dynamic_cast<const GuidedLoftOp*>(op)) {
        if (gl->getBaseSketchId() >= 0) src.insert(gl->getBaseSketchId());
        for (int k = 0; k < gl->railSketchCount(); ++k)
            if (gl->getRailSketchId(k) >= 0) src.insert(gl->getRailSketchId(k));
    }
    return src;
}

// Which bodies each sketch drives: sketch id -> bodies touched (created or
// modified) by any step built from it. Application::sketchBodyLinks() memoizes
// this; it lives here so headless code (and tests) can ask the same question.
inline std::map<int, std::set<int>> computeSketchBodyLinks(const History& history) {
    std::map<int, std::set<int>> links;
    const int n = history.stepCount();
    for (int i = 0; i < n; ++i) {
        const Operation* op = history.getStep(i);
        const std::set<int> srcSketches = opSourceSketches(op);
        if (srcSketches.empty()) continue;
        OperationDiff diff = op->captureDiff();
        std::set<int> bodies(diff.created.begin(), diff.created.end());
        for (const auto& [id, _] : diff.modifiedBefore) bodies.insert(id);
        for (int s : srcSketches)
            links[s].insert(bodies.begin(), bodies.end());
    }
    return links;
}

// Sketches that sit on a body `sketchId` drives, and so have to travel with it
// when `sketchId` is moved - and, one level down, sketches on the bodies THOSE
// drive. A sketch is "on" a body when its source body is that body (drawn on one
// of its faces). Moving Sketch 1 moves the body built from it; leaving Sketch 2
// (drawn on that body) behind strands every feature built from Sketch 2, which
// then fails to follow and is switched off (issue #185).
//
// The relation only points FORWARD in the history: a follower's first feature
// comes after the first feature of the sketch it follows. Without that, the
// app's "re-adopt a severed body link" heal (a base sketch that ends up coplanar
// with the face of the body it built becomes "hosted" on it) made Sketch 1 and
// Sketch 2 hosts of each other, and moving the top sketch dragged the base one.
// `exclude`: sketches never reported (the ones already being moved themselves).
// Ascending ids; `sketchId` itself is never in the result.
inline std::vector<int> hostedFollowerSketches(const std::map<int, std::set<int>>& links,
                                               const History& history,
                                               const Document& doc, int sketchId,
                                               const std::set<int>& exclude = {}) {
    std::map<int, int> firstStep;                 // sketch -> its earliest feature step
    for (int i = 0; i < history.stepCount(); ++i)
        for (int sid : opSourceSketches(history.getStep(i))) firstStep.emplace(sid, i);
    auto stepOf = [&](int sid) { auto it = firstStep.find(sid); return it == firstStep.end() ? -1 : it->second; };

    std::set<int> seen{sketchId};
    std::vector<int> frontier{sketchId};
    const std::vector<int> all = doc.getAllSketchIds();
    while (!frontier.empty()) {
        const int cur = frontier.back();
        frontier.pop_back();
        auto it = links.find(cur);
        if (it == links.end()) continue;
        const int curStep = stepOf(cur);
        for (int sid : all) {
            if (seen.count(sid) || exclude.count(sid)) continue;
            auto sk = doc.getSketch(sid);
            if (!sk || sk->getSourceBody() < 0 || !it->second.count(sk->getSourceBody())) continue;
            // A sketch that already has features, none of them after the one it
            // would follow, is not hosted on it. (A sketch with no feature yet
            // can drive nothing, so it cannot form a cycle - it just travels.)
            if (const int st = stepOf(sid); st >= 0 && st <= curStep) continue;
            seen.insert(sid);
            frontier.push_back(sid);
        }
    }
    seen.erase(sketchId);
    return std::vector<int>(seen.begin(), seen.end());
}

// Hand a sketch move the sketches that must travel with it (see
// hostedFollowerSketches). Nothing is added for a sketch that is detached from
// its body - its move deliberately leaves the body where it is.
inline void addHostedFollowers(SketchTransformOp& op,
                               const std::map<int, std::set<int>>& links,
                               const History& history,
                               const Document& doc, int sketchId,
                               const std::set<int>& exclude = {}) {
    auto sk = doc.getSketch(sketchId);
    if (!sk || sk->isDetachedFromBody()) return;
    for (int f : hostedFollowerSketches(links, history, doc, sketchId, exclude))
        op.addFollower(f);
}

// A cascade that can't make a step follow a sketch edit switches it off
// (Operation::isCascadeSuspended). That used to be permanent: drag a lathe
// profile across its axis by half a millimetre and the Lathe - and the cut and
// union built on it - went dark, and stayed dark after the profile was dragged
// back. This gives such steps another try on the next cascade of their sketch.
//
// Flags every suspended applied step from the first one built from `sketchId`
// onward as REVIVING (enabled, not yet applied) and returns their indices,
// ascending; empty when nothing built from this sketch is suspended. The
// caller replays the chain from the first one and then calls settleRevived().
// Steps suspended for an unrelated sketch are left alone.
inline std::vector<int> reviveSuspendedFor(History& history, int sketchId) {
    std::vector<int> revived;
    bool started = false;
    const int applied = history.currentStep();
    for (int i = 0; i <= applied && i < history.stepCount(); ++i) {
        Operation* op = const_cast<Operation*>(history.getStep(i));
        if (!op || op->isEnabled() || !op->isCascadeSuspended()) continue;
        if (!started && !opBuiltFromSketch(op, sketchId)) continue;
        started = true;
        op->setEnabled(true);
        op->setReviving(true);
        revived.push_back(i);
    }
    return revived;
}

// Ends a revival started by reviveSuspendedFor(). `replayOk`: the replay
// succeeded, so every step still reviving ran and is simply applied again.
// Otherwise they go back to suspended, for the next attempt. Returns how many
// steps came back.
inline int settleRevived(History& history, const std::vector<int>& revived,
                         bool replayOk) {
    int back = 0;
    for (int i : revived) {
        Operation* op = const_cast<Operation*>(history.getStep(i));
        if (!op || !op->isReviving()) continue;
        op->setReviving(false);
        if (replayOk) {
            op->setCascadeSuspended(false);
            ++back;
        } else {
            op->setEnabled(false);
        }
    }
    return back;
}

// Fires `cascade(sketchId)` for every sketch that needs its driven body
// resynced after an undo()/redo() call, given:
// - `appliedIdx`: History::lastUndoneStep()/lastRedoneStep() from that call
//   (< 0 means nothing was actually applied - fires nothing, unconditionally)
// - `appliedOp`: history.getStep(appliedIdx) (null when appliedIdx < 0)
// - `doc`: for sketch-id resolution
// - `activeSketchId`: the in-progress sketch session's id, or -1 when not in
//   sketch mode / no active sketch (keeps an in-progress sketch's driven body
//   in sync even for an upstream op that isn't itself a sketch op)
// Fires each distinct sketch id at most once (the active sketch and the
// applied op's sketch are often the same id).
template <typename Cascade>
void dispatchUndoRedoCascade(int appliedIdx, const Operation* appliedOp,
                             const Document& doc, int activeSketchId,
                             Cascade&& cascade) {
    if (appliedIdx < 0) return; // nothing was actually applied - never cascade
    // A Fill-tool step moved no geometry: neither its sketch nor the active
    // one has anything new to push into a body.
    if (auto* se = dynamic_cast<const SketchEditOp*>(appliedOp); se && se->onlyFillsChanged())
        return;
    int cascaded = -1;
    if (activeSketchId >= 0) {
        cascade(activeSketchId);
        cascaded = activeSketchId;
    }
    if (int sid = sketchIdForCascade(appliedOp, doc); sid >= 0 && sid != cascaded)
        cascade(sid);
}

} // namespace materializr
