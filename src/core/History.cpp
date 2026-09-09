#include "History.h"
#include "EventBus.h"
#include "Events.h"
#include "Verbose.h"
#include "../modeling/Sketch.h"
#include "../modeling/SketchEditOp.h"
#include <cstdio>
#include <map>
#include <set>

History::History() = default;

bool History::pushOperation(std::unique_ptr<Operation> op, Document& doc) {
    ++m_revision;
    if (!op) {
        return false;
    }

    // THREADS ARE A FINISHING PASS - enforced by REFLOW: an op targeting a
    // thread-modified body is reordered beneath the thread(s) via
    // insertStepAndReplay (non-thread steps replay first, then the op
    // against clean geometry, then the threads re-cut parametrically on the
    // result). Between 2026-06 and 2026-07 this was a hard refusal
    // ("threads-last discipline") because the re-cut ran a ~minute per-turn
    // recompute synchronously on the main thread. Two things removed that
    // cost: ThreadOp's async recut hook (the re-cut lands from a worker;
    // the body shows unthreaded for a moment) and the swept-profile fast
    // path (~200ms for Standard/Rounded). If the reflow can't land (baked
    // reload snapshot on the body / a step failed to replay - state is
    // restored inside insertStepAndReplay), fall back to the old refusal
    // with guidance rather than running the op directly against the
    // thread's helicoid faces (kernel garbage).
    {
        int at = reflowInsertionIndex(*op);
        if (at >= 0) {
            const std::string nm = op->name();
            std::fprintf(stderr, "[History] reflowing '%s' beneath the "
                                 "Thread at step %d (threads re-cut last)\n",
                         nm.c_str(), at);
            if (insertStepAndReplay(at, std::move(op), doc)) return true;
            std::fprintf(stderr, "[History] '%s' declined: reflow beneath "
                                 "the Thread step failed. Delete the Thread "
                                 "step, make this change, then re-apply the "
                                 "thread.\n", nm.c_str());
            if (m_threadsLastDecline) m_threadsLastDecline();
            return false;
        }
    }

    // SHELLS auto-reflow (unlike threads no refusal - a shell re-execute is
    // sub-second): a face transform on a shelled body applies to the
    // PRE-SHELL solid and the shell re-runs on the moved body, giving the
    // result the user means ("the order flipped"). If the reflow can't land
    // (rolled back + restored inside insertStepAndReplay), fail cleanly -
    // the direct path would only hit the hollow-body corruption guards.
    {
        int at = shellReflowIndex(*op);
        if (at >= 0) {
            std::fprintf(stderr, "[History] reflowing '%s' beneath the Shell "
                                 "at step %d (face ops apply pre-shell)\n",
                         op->name().c_str(), at);
            return insertStepAndReplay(at, std::move(op), doc);
        }
    }

    // Execute the operation
    if (!op->execute(doc)) {
        return false;
    }
    op->rememberGoodParams();

    // Clear any redo stack (operations beyond current index)
    if (m_currentIndex + 1 < static_cast<int>(m_operations.size())) {
        m_operations.erase(m_operations.begin() + m_currentIndex + 1, m_operations.end());
    }

    // Push the operation
    m_operations.push_back(std::move(op));
    m_currentIndex = static_cast<int>(m_operations.size()) - 1;

    if (m_eventBus) m_eventBus->publish(materializr::HistoryStepEvent{m_currentIndex, false});
    return true;
}

void History::pushExecuted(std::unique_ptr<Operation> op) {
    ++m_revision;
    if (!op) return;
    op->rememberGoodParams(); // already-applied params are by definition good
    if (m_currentIndex + 1 < static_cast<int>(m_operations.size())) {
        m_operations.erase(m_operations.begin() + m_currentIndex + 1, m_operations.end());
    }
    m_operations.push_back(std::move(op));
    m_currentIndex = static_cast<int>(m_operations.size()) - 1;
    if (m_eventBus) m_eventBus->publish(materializr::HistoryStepEvent{m_currentIndex, false});
}

bool History::canUndo() const {
    // m_undoFloor (default -1) keeps this identical to `m_currentIndex >= 0`
    // outside a sketch; inside one it stops undo at the sketch-entry step so no
    // path can roll the host body back under the live sketch. See setUndoFloor.
    return m_currentIndex >= 0 && m_currentIndex > m_undoFloor;
}

bool History::canRedo() const {
    return m_currentIndex < static_cast<int>(m_operations.size()) - 1;
}

bool History::undo(Document& doc) {
    ++m_revision;
    if (!canUndo()) {
        std::fprintf(stderr, "[History] undo: nothing to undo (currentIndex=%d)\n",
                     m_currentIndex);
        return false;
    }

    // A DISABLED step at the tip was never applied (replayAll skips disabled ops),
    // so calling its undo() would revert geometry that never existed. Walk the tip
    // down past any disabled steps and undo the last ACTUALLY-APPLIED (enabled) one.
    int idx = m_currentIndex;
    while (idx > m_undoFloor && !m_operations[idx]->isEnabled()) idx--;
    if (idx <= m_undoFloor) {
        // Only disabled (never-applied) steps sit above the floor - nothing to undo.
        std::fprintf(stderr, "[History] undo: only disabled steps above floor (currentIndex=%d)\n",
                     m_currentIndex);
        return false;
    }

    Operation* op = m_operations[idx].get();
    // Per-undo trace is --verbose only; the FAILED path below stays loud.
    if (materializr::isVerbose())
        std::fprintf(stderr, "[History] undo step %d '%s' (type=%s reloaded=%d enabled=%d)\n",
                     idx, op->name().c_str(), op->typeId().c_str(),
                     op->isReloaded() ? 1 : 0, op->isEnabled() ? 1 : 0);
    if (!op->undo(doc)) {
        std::fprintf(stderr, "[History] undo FAILED at step %d '%s' - op->undo() "
                             "returned false; staying at this step\n",
                     idx, op->name().c_str());
        return false;
    }

    m_currentIndex = idx - 1;
    // Manual undo means the user is steering the applied range themselves -
    // drop any pending auto-recovery so a later edit doesn't surprise-redo
    // steps they deliberately walked back past.
    m_failedReplayAt = -1;
    if (m_eventBus) m_eventBus->publish(materializr::HistoryStepEvent{m_currentIndex, true});
    return true;
}

bool History::redo(Document& doc) {
    ++m_revision;
    if (!canRedo()) {
        return false;
    }

    // Skip DISABLED steps: they are suppressed, so executing one would re-apply
    // geometry the user turned off. Advance to the next ENABLED step and run it.
    const int n = static_cast<int>(m_operations.size());
    int idx = m_currentIndex + 1;
    while (idx < n && !m_operations[idx]->isEnabled()) idx++;
    if (idx >= n) {
        // Only disabled steps remain in the redo range - consume them (advance the
        // tip past them, no execution) so Ctrl+Y doesn't keep firing with no effect.
        m_currentIndex = n - 1;
        if (m_eventBus) m_eventBus->publish(materializr::HistoryStepEvent{m_currentIndex, false});
        return true;
    }

    Operation* op = m_operations[idx].get();
    if (!op->execute(doc)) {
        m_failedReplayAt = idx;
        return false; // leave the tip below the failed step
    }

    m_currentIndex = idx;
    op->rememberGoodParams();
    if (m_failedReplayAt >= 0 && m_currentIndex >= m_failedReplayAt) {
        m_failedReplayAt = -1; // the previously-failed step recomputed fine
    }
    if (m_eventBus) m_eventBus->publish(materializr::HistoryStepEvent{m_currentIndex, false});
    return true;
}

int History::stepCount() const {
    return static_cast<int>(m_operations.size());
}

int History::currentStep() const {
    return m_currentIndex;
}

const Operation* History::getStep(int index) const {
    if (index < 0 || index >= static_cast<int>(m_operations.size())) {
        return nullptr;
    }
    return m_operations[index].get();
}

void History::propagateSketchValueEdits(int editedStep, Document& doc) {
    ++m_revision;
    if (editedStep < 0 || editedStep >= static_cast<int>(m_operations.size()))
        return;
    auto* edited =
        dynamic_cast<materializr::SketchEditOp*>(m_operations[editedStep].get());
    if (!edited) return;
    const auto& radii = edited->editedCircleRadii();
    if (radii.empty()) return;
    auto tgt = edited->getTarget();
    int sid = tgt ? doc.findSketchId(tgt.get()) : -1;
    if (sid < 0) return;
    for (int i = editedStep + 1; i < static_cast<int>(m_operations.size()); ++i) {
        auto* se = dynamic_cast<materializr::SketchEditOp*>(m_operations[i].get());
        if (!se) continue;
        auto t2 = se->getTarget();
        if (!t2 || doc.findSketchId(t2.get()) != sid) continue;
        for (const auto& [cid, r] : radii)
            se->applyCircleRadiusToSnapshots(cid, r);
    }
    // The edit is now baked into the downstream snapshots; clear so a later,
    // unrelated edit on this same step doesn't re-propagate a stale value.
    edited->clearEditedCircleRadii();
}

bool History::editStep(int index, Document& doc, bool transactional) {
    m_lastEditFailStep = -1;
    ++m_revision;
    if (index < 0 || index >= static_cast<int>(m_operations.size())) {
        return false;
    }

    // Snapshot the body IDs BEFORE any undo/replay. Used at the end to detect
    // and prune orphan bodies created when a downstream op has lost its
    // reuseBodyIds (e.g., a push/pull op whose created-body tracking was
    // corrupted in the saved file). Only applied in non-transactional (preview)
    // mode - transactional failures are already handled by restoreSnapshot.
    const std::vector<int> preEditBodyIds = doc.getAllBodyIds();

    // Transactional safety: snapshot the whole model up front so a replay that
    // fails partway (a downstream fillet whose edges can't re-bind after the
    // edited geometry moved, etc.) can be fully reverted - an edit must never
    // strand a half-built model. TopoDS_Shape is a cheap handle; sketches copy
    // by value. Only done when asked (one-shot Apply paths), not per preview frame.
    const int savedIndex = m_currentIndex;
    std::map<int, TopoDS_Shape> bodySnap;
    std::map<int, materializr::Sketch> sketchSnap;
    if (transactional) {
        for (int id : doc.getAllBodyIds()) bodySnap[id] = doc.getBody(id);
        for (int sid : doc.getAllSketchIds())
            if (auto sk = doc.getSketch(sid)) sketchSnap.emplace(sid, *sk);
        // Op-internal edit state too: ops that SUCCEED during a replay that
        // later fails have re-resolved their stored edges/refs against bodies
        // the rollback below is about to discard. Restoring bodies but not
        // that state wedges the step - the next attempt resolves against
        // geometry that no longer exists (fails where a fresh session works).
        for (auto& op : m_operations) op->snapshotEditState();
    }
    auto restoreSnapshot = [&]() {
        std::set<int> want;
        for (const auto& [id, shp] : bodySnap) { doc.putBody(id, shp); want.insert(id); }
        for (int id : doc.getAllBodyIds()) if (!want.count(id)) doc.removeBody(id);
        for (const auto& [sid, sk] : sketchSnap)
            if (auto live = doc.getSketch(sid)) *live = sk;
        for (auto& op : m_operations) op->restoreEditState();
        m_currentIndex = savedIndex;
    };

    int limit = m_currentIndex;
    if (m_breakpoint >= 0 && m_breakpoint < limit) {
        limit = m_breakpoint;
    }

    if (index > limit) {
        // The step isn't currently applied - e.g. it was suspended by an
        // earlier failed recompute (fillet grew, chamfer edge vanished) and
        // the user is editing ITS parameters to fix it. Roll FORWARD to it,
        // executing the intervening steps, instead of refusing.
        for (int i = limit + 1; i <= index; ++i) {
            Operation* op = m_operations[i].get();
            if (op->isEnabled()) {
                if (!op->execute(doc)) {
                    if (transactional) { restoreSnapshot(); return false; }
                    m_failedReplayAt = i;
                    return false;
                }
                op->rememberGoodParams();
            }
            m_currentIndex = i;
        }
        if (m_failedReplayAt >= 0 && m_currentIndex >= m_failedReplayAt) {
            m_failedReplayAt = -1;
        }
        return true;
    }

    // Rebuild IN PLACE rather than clearing the document: clearing would also
    // wipe bodies that aren't operations (the base/imported bodies) and reset
    // body ids, so dependent ops (which reference body ids) would fail. Instead
    // roll back to just before `index` by undoing the applied ops in reverse,
    // then re-execute from `index` forward so the edited parameters take effect.
    for (int i = limit; i >= index; --i) {
        Operation* op = m_operations[i].get();
        if (op->isEnabled()) op->undo(doc);
    }
    bool editRejected = false;
    for (int i = index; i <= limit; ++i) {
        Operation* op = m_operations[i].get();
        if (op->isEnabled()) {
            if (!op->execute(doc)) {
                // If the EDITED step itself rejects its new values (e.g. a
                // fillet radius its host geometry can't carry), restore the
                // parameters from its last successful execute and reapply -
                // the model stays as it was instead of stranding this step
                // and everything above it (where the next Ctrl+Z would hit
                // the step below: "undo deleted the whole body"). The UI
                // re-reads the op, so the value visibly snaps back.
                //
                // PREVIEW MODE ONLY: a transactional caller holds a full
                // pre-edit snapshot, which restores the EXACT prior model -
                // a lastGoodParams REBUILD merely approximates it (fallback-
                // built blends are path-dependent: rebuilding "the same"
                // chamfer can produce different downstream geometry, silently
                // swapping the model under a failed edit).
                const std::string& good = op->lastGoodParams();
                if (i == index && !transactional && !good.empty() &&
                    op->deserializeParams(good) && op->execute(doc)) {
                    std::fprintf(stderr,
                        "[history-dbg] editStep: step %d rejected new params, "
                        "restored lastGoodParams\n", i);
                    editRejected = true;
                    continue;
                }
                std::fprintf(stderr,
                    "[history-dbg] editStep: HARD FAILURE at step %d "
                    "(edited=%d transactional=%d)\n", i, index, transactional);
                m_lastEditFailStep = i;
                if (transactional) { restoreSnapshot(); return false; }
                m_currentIndex = i - 1;
                m_failedReplayAt = i;
                return false;
            }
            op->rememberGoodParams();
        }
    }
    if (editRejected) return false; // model intact, but the edit didn't apply

    // If a PREVIOUS edit knocked steps out (failed recompute left a suspended
    // tail), retry them now that the upstream geometry changed again - this is
    // what makes "grow the fillet too far, chamfer dies, shrink the fillet
    // back" bring the chamfer back automatically. Stops at the first step
    // that still fails (keeping the flag) without failing THIS edit.
    if (m_failedReplayAt >= 0) {
        int cap = static_cast<int>(m_operations.size()) - 1;
        if (m_breakpoint >= 0 && m_breakpoint < cap) cap = m_breakpoint;
        bool cleared = true;
        for (int i = limit + 1; i <= cap; ++i) {
            Operation* op = m_operations[i].get();
            if (op->isEnabled()) {
                if (!op->execute(doc)) {
                    m_failedReplayAt = i;
                    cleared = false;
                    break;
                }
                op->rememberGoodParams();
            }
            m_currentIndex = i;
        }
        if (cleared) m_failedReplayAt = -1;
    }

    // Orphan-body cleanup (non-transactional / preview mode only).
    //
    // Problem: if a push/pull op's body-ID state was lost when the project
    // file was saved mid-undo (m_createdBodyIds was empty at save time so the
    // body ended up in HISTORY_INITIAL_COUNT instead of the op's diff), then
    // on reload the op has empty m_reuseBodyIds. Its undo() is a no-op (leaves
    // the phantom initialState body in the doc) and its execute() creates a
    // FRESH body with a new ID rather than reusing the original. The phantom
    // stays AND a duplicate appears - causing missing or misplaced geometry.
    //
    // Fix: any body ID that wasn't in the document before this editStep but IS
    // there now is an orphan created by a stateless op. Remove it so only the
    // correctly-managed bodies remain. The op retains its m_createdBodyIds =
    // {new_id} and m_reuseBodyIds will be set to {new_id} on the next undo(),
    // so subsequent preview frames recreate and re-clean the same orphan via the
    // tombstone mechanism - cosmetically invisible to the user.
    //
    // Transactional (commit) mode: restoreSnapshot() handles cleanup on failure;
    // on success the history should be clean - skip this guard.
    if (!transactional) {
        std::set<int> preSet(preEditBodyIds.begin(), preEditBodyIds.end());
        for (int id : doc.getAllBodyIds()) {
            if (!preSet.count(id)) {
                std::fprintf(stderr,
                    "[editStep] orphan body %d removed (appeared during replay, "
                    "not in pre-edit set of %zu) - likely a push/pull op with "
                    "lost body-ID state from a mid-undo save.\n",
                    id, preSet.size());
                doc.removeBody(id);
            }
        }
    }

    // Note: we deliberately don't publish HistoryStepEvent here. editStep
    // is initiated from explicit user UI (HistoryPanel's Apply Changes),
    // which publishes its own SketchEditedEvent when appropriate - better
    // signal-to-noise than a generic step event that also fires for
    // unrelated history shuffles (push/pull preview undos, etc).
    return true;
}

bool History::removeStep(int index, Document& doc) {
    ++m_revision;
    int count = static_cast<int>(m_operations.size());
    if (index < 0 || index >= count) return false;

    // If the step is beyond the current state (in the redo region) it has no
    // effect on the document right now - just drop it.
    if (index > m_currentIndex) {
        m_operations.erase(m_operations.begin() + index);
        if (m_breakpoint > index) m_breakpoint--;
        else if (m_breakpoint == index) m_breakpoint = -1;
        return true;
    }

    // Roll the document back to just before `index` by undoing applied ops.
    for (int i = m_currentIndex; i >= index; --i) {
        Operation* op = m_operations[i].get();
        if (op->isEnabled()) op->undo(doc);
    }

    // Detach the op so we can restore it if a later dependent op fails.
    std::unique_ptr<Operation> removed = std::move(m_operations[index]);
    m_operations.erase(m_operations.begin() + index);

    int savedCurrent = m_currentIndex;
    int savedBreak = m_breakpoint;
    m_currentIndex--; // one fewer applied op
    if (m_breakpoint > index) m_breakpoint--;
    else if (m_breakpoint == index) m_breakpoint = -1;

    // Re-execute the remaining ops from `index` forward.
    for (int i = index; i <= m_currentIndex; ++i) {
        Operation* op = m_operations[i].get();
        if (op->isEnabled()) {
            if (!op->execute(doc)) {
                // Conflict: a later op depended on the removed one. Roll back the
                // partial rebuild, reinsert the removed op, and replay the
                // original chain so the model is left exactly as it was.
                for (int j = i - 1; j >= index; --j) {
                    Operation* o = m_operations[j].get();
                    if (o->isEnabled()) o->undo(doc);
                }
                m_operations.insert(m_operations.begin() + index, std::move(removed));
                m_currentIndex = savedCurrent;
                m_breakpoint = savedBreak;
                for (int j = index; j <= m_currentIndex; ++j) {
                    Operation* o = m_operations[j].get();
                    if (o->isEnabled()) o->execute(doc);
                }
                return false;
            }
        }
    }

    if (m_eventBus) m_eventBus->publish(materializr::HistoryStepEvent{m_currentIndex, true});
    return true;
}

bool History::setStepEnabled(int index, bool enabled, Document& doc) {
    ++m_revision;
    int count = static_cast<int>(m_operations.size());
    if (index < 0 || index >= count) return false;
    Operation* target = m_operations[index].get();
    if (target->isEnabled() == enabled) return true; // no change

    // Above the applied tip (redo region / breakpoint-suppressed): the step
    // isn't in the document right now, so just flip the flag - the next
    // redo/replay will honor it.
    if (index > m_currentIndex) {
        target->setEnabled(enabled);
        return true;
    }

    // Roll the document back to just before `index` by undoing the applied ops
    // in reverse. Uses the CURRENT (pre-toggle) flags: a currently-enabled
    // target gets undone here; a currently-disabled one (we're enabling it) was
    // never applied, so it's correctly skipped.
    for (int i = m_currentIndex; i >= index; --i) {
        Operation* op = m_operations[i].get();
        if (op->isEnabled()) op->undo(doc);
    }

    target->setEnabled(enabled);

    // Re-execute from `index` forward, IN PLACE. No doc.clear(), so base /
    // imported bodies that aren't operations (the starting box a lone push/pull
    // edits) survive. Disabled ops are skipped; an op that can no longer rebuild
    // because the geometry it referenced was suppressed upstream is skipped too
    // (cascade suppression) and recorded so the UI can flag the partial result.
    m_failedReplayAt = -1;
    int firstFail = -1;
    for (int i = index; i <= m_currentIndex; ++i) {
        Operation* op = m_operations[i].get();
        if (op->isEnabled()) {
            if (!op->execute(doc)) {
                if (firstFail < 0) firstFail = i;
                continue;
            }
            op->rememberGoodParams();
        }
    }
    if (firstFail >= 0) m_failedReplayAt = firstFail;

    if (m_eventBus)
        m_eventBus->publish(materializr::HistoryStepEvent{m_currentIndex, true});
    return firstFail < 0;
}

void History::setBreakpoint(int index) {
    m_breakpoint = index;
}

int History::getBreakpoint() const {
    return m_breakpoint;
}

bool History::replayAll(Document& doc) {
    ++m_revision;
    doc.clear();

    int limit = static_cast<int>(m_operations.size()) - 1;
    if (m_breakpoint >= 0 && m_breakpoint < limit) {
        limit = m_breakpoint;
    }

    // Called only by the history Disable/Enable toggle. Suppressing a feature
    // strips the geometry downstream ops were built on, so some of them can no
    // longer recompute. Rather than ABORT the whole replay at the first such
    // failure - which blanks the viewport and reads as "the model is gone" -
    // SKIP the failed op and keep going. A dependent op that needs the
    // suppressed geometry simply fails and is skipped too (cascade suppression),
    // while geometry that's independent of the disabled step still builds. Re-
    // enabling the step replays cleanly and brings everything back, so the
    // operation is always recoverable. The first failure is recorded so the UI
    // can flag that the rebuild was partial.
    m_failedReplayAt = -1;
    int firstFailure = -1;
    for (int i = 0; i <= limit; ++i) {
        Operation* op = m_operations[i].get();
        if (op->isEnabled()) {
            if (!op->execute(doc)) {
                if (firstFailure < 0) firstFailure = i;
                continue; // skip this op, keep replaying the rest
            }
            op->rememberGoodParams();
        }
    }

    m_currentIndex = limit;
    if (firstFailure >= 0) {
        m_failedReplayAt = firstFailure;
        return false;
    }
    return true;
}

void History::clear() {
    ++m_revision;
    m_operations.clear();
    m_currentIndex = -1;
    m_breakpoint = -1;
    m_failedReplayAt = -1;
}

void History::dropRedoTail() {
    ++m_revision;
    if (m_currentIndex + 1 < static_cast<int>(m_operations.size()))
        m_operations.erase(m_operations.begin() + m_currentIndex + 1,
                           m_operations.end());
}

const std::vector<std::unique_ptr<Operation>>& History::operations() const {
    return m_operations;
}

bool History::isBodyThreaded(int bodyId) const {
    if (bodyId < 0) return false;
    int limit = m_currentIndex;
    for (int i = 0; i <= limit && i < static_cast<int>(m_operations.size());
         ++i) {
        const Operation* s = m_operations[i].get();
        // A DISABLED thread is not in the model, so the body is not threaded:
        // this gate only exists to make callers avoid the helicoid (ghost
        // preview in Push/Pull, no live preview in Resize Cylindrical), and
        // there is nothing to avoid. Missing the isEnabled() check left every
        // later op degrading its preview for a thread the user had switched
        // off - while reflowInsertionIndex(), which DOES check, saw no thread
        // to reorder beneath. isBodyShelled() below always had it right.
        if (!s || !s->isEnabled() || s->kind() != Operation::Kind::Thread) continue;
        // ThreadOp doesn't override plannedBodyIds() (returns {}); the body it
        // modified is recorded in its diff. Mirror reflowInsertionIndex's
        // touchesPlanned() so the up-front refusal matches the commit-time one.
        OperationDiff d = s->captureDiff();
        for (const auto& [id, shp] : d.modifiedBefore)
            if (id == bodyId) return true;
        for (int id : d.created)
            if (id == bodyId) return true;
    }
    return false;
}

bool History::isBodyShelled(int bodyId) const {
    if (bodyId < 0) return false;
    int limit = m_currentIndex;
    for (int i = 0; i <= limit && i < static_cast<int>(m_operations.size());
         ++i) {
        const Operation* s = m_operations[i].get();
        if (!s || !s->isEnabled() || s->kind() != Operation::Kind::Shell) continue;
        OperationDiff d = s->captureDiff();
        for (const auto& [id, shp] : d.modifiedBefore)
            if (id == bodyId) return true;
        for (int id : d.created)
            if (id == bodyId) return true;
    }
    return false;
}

int History::reflowInsertionIndex(const Operation& op) const {
    if (op.kind() == Operation::Kind::Thread) return -1; // stacking threads is fine as-is
    std::vector<int> planned = op.plannedBodyIds();
    if (planned.empty()) return -1;

    int limit = m_currentIndex;
    if (m_breakpoint >= 0 && m_breakpoint < limit) limit = m_breakpoint;

    auto touchesPlanned = [&](const Operation* s) {
        OperationDiff d = s->captureDiff();
        for (const auto& [id, shp] : d.modifiedBefore)
            for (int p : planned) if (p == id) return true;
        for (int id : d.created)
            for (int p : planned) if (p == id) return true;
        for (const auto& [id, shp] : d.deletedBefore)
            for (int p : planned) if (p == id) return true;
        return false;
    };

    // Walk down from the top, SKIPPING steps unrelated to the op's bodies
    // (real histories always have sketches / other-body work above the
    // thread). Reorder beneath the deepest thread that touched a planned
    // body. NON-thread touching steps above that thread no longer block:
    // insertStepAndReplay replays them BEFORE the new op (thread-last
    // partition), so an op that depends on their results - e.g. a Boolean
    // subtract whose tool body was push/pulled into existence AFTER the
    // thread - still sees them applied. (The old stop-cold rule made that
    // Boolean run directly against the threaded rod: kernel garbage.)
    int insertAt = -1;
    for (int i = limit; i >= 0; --i) {
        const Operation* s = m_operations[i].get();
        if (!s->isEnabled()) continue;
        if (s->kind() == Operation::Kind::Thread && touchesPlanned(s)) insertAt = i;
    }
    return insertAt;
}

int History::shellReflowIndex(const Operation& op) const {
    if (op.kind() != Operation::Kind::MoveFace) return -1;
    std::vector<int> planned = op.plannedBodyIds();
    if (planned.empty()) return -1;

    int limit = m_currentIndex;
    if (m_breakpoint >= 0 && m_breakpoint < limit) limit = m_breakpoint;

    auto touchesPlanned = [&](const Operation* s) {
        OperationDiff d = s->captureDiff();
        for (const auto& [id, shp] : d.modifiedBefore)
            for (int p : planned) if (p == id) return true;
        for (int id : d.created)
            for (int p : planned) if (p == id) return true;
        for (const auto& [id, shp] : d.deletedBefore)
            for (int p : planned) if (p == id) return true;
        return false;
    };

    // Reflow beneath the DEEPEST shell that touched a planned body, so the
    // face transform applies to the pre-shell solid and every shell above it
    // re-hollows the moved geometry.
    int insertAt = -1;
    for (int i = limit; i >= 0; --i) {
        const Operation* s = m_operations[i].get();
        if (!s->isEnabled()) continue;
        if (s->kind() == Operation::Kind::Shell && touchesPlanned(s)) insertAt = i;
    }
    return insertAt;
}

bool History::insertStepAndReplay(int index, std::unique_ptr<Operation> op,
                                  Document& doc) {
    ++m_revision;
    int limit = m_currentIndex;
    if (m_breakpoint >= 0 && m_breakpoint < limit) limit = m_breakpoint;
    if (index < 0 || index > limit) return false;

    // Only steps that TOUCH the op's bodies are rolled back and replayed.
    // Unrelated steps stay applied - critically, a reloaded project's baked
    // full-document snapshot steps must NOT re-execute, or their snapshots
    // resurrect the pre-op world and silently erase the inserted op's result
    // (Steve: "it didn't work, but it didn't crash").
    std::vector<int> planned = op->plannedBodyIds();
    auto touches = [&](const Operation* s) {
        OperationDiff d = s->captureDiff();
        for (const auto& [id, shp] : d.modifiedBefore)
            for (int p : planned) if (p == id) return true;
        for (int id : d.created)
            for (int p : planned) if (p == id) return true;
        for (const auto& [id, shp] : d.deletedBefore)
            for (int p : planned) if (p == id) return true;
        return false;
    };
    std::vector<int> touched; // indices in [index..limit], ascending
    for (int i = index; i <= limit; ++i) {
        Operation* s = m_operations[i].get();
        if (!s->isEnabled() || !touches(s)) continue;
        if (s->isReloaded()) {
            // A baked reload snapshot on this body can't recompute - replaying
            // it would clobber the op's result. Decline the whole reflow; the
            // caller falls back to the direct path (fast failure, no hang).
            std::fprintf(stderr, "[History] reflow declined: step %d '%s' is "
                                 "a baked reload snapshot on the same body\n",
                         i, s->name().c_str());
            return false;
        }
        touched.push_back(i);
    }

    // Roll back the touched steps (deepest last).
    for (int k = static_cast<int>(touched.size()) - 1; k >= 0; --k) {
        m_operations[touched[k]]->undo(doc);
    }

    // EXTRACT the touched ops (highest index first so the indices stay
    // valid), preserving their original relative order. They go back in
    // THREAD-LAST order: non-thread steps first (rebuilding e.g. the tool
    // body a Boolean depends on), then the new op, then the threads - so
    // every boolean in the chain runs against clean geometry and the
    // threads re-cut parametrically at the end.
    std::vector<std::unique_ptr<Operation>> extracted;
    for (int k = static_cast<int>(touched.size()) - 1; k >= 0; --k) {
        extracted.insert(extracted.begin(),
                         std::move(m_operations[touched[k]]));
        m_operations.erase(m_operations.begin() + touched[k]);
    }
    // Finishing passes replay AFTER the new op. Threads always; shells only
    // when the new op is a face transform (the shell-reflow case) - for any
    // other insertion a shell in the window keeps its original position, so
    // e.g. a boolean that ran on the hollow body still does.
    const bool shellIsFinishing = (op->kind() == Operation::Kind::MoveFace);
    std::vector<size_t> ntIdx, thIdx; // partition, original order kept
    for (size_t k = 0; k < extracted.size(); ++k) {
        const std::string t = extracted[k]->typeId();
        if (t == "thread" || (shellIsFinishing && t == "shell"))
            thIdx.push_back(k);
        else ntIdx.push_back(k);
    }

    // Bail-out: re-execute everything in ORIGINAL order against the rolled-
    // back state and re-insert the block at `index` (untouched window steps
    // end up after the block, but they share no bodies with it).
    auto restoreOriginal = [&]() {
        for (auto& s : extracted) {
            if (s->execute(doc)) s->rememberGoodParams();
        }
        m_operations.insert(m_operations.begin() + index,
                            std::make_move_iterator(extracted.begin()),
                            std::make_move_iterator(extracted.end()));
    };

    // 1. Replay the non-thread steps the op may depend on.
    for (size_t n = 0; n < ntIdx.size(); ++n) {
        Operation* s = extracted[ntIdx[n]].get();
        if (!s->execute(doc)) {
            std::fprintf(stderr, "[History] reflow declined: step '%s' "
                                 "failed against pre-thread geometry\n",
                         s->name().c_str());
            for (size_t u = n; u-- > 0;) extracted[ntIdx[u]]->undo(doc);
            restoreOriginal();
            return false;
        }
        s->rememberGoodParams();
    }

    // 2. The new op runs against clean geometry.
    if (!op->execute(doc)) {
        for (size_t u = ntIdx.size(); u-- > 0;) extracted[ntIdx[u]]->undo(doc);
        restoreOriginal();
        return false;
    }
    op->rememberGoodParams();

    // 3. Splice the reordered block in: [non-threads..., op, threads...].
    std::vector<std::unique_ptr<Operation>> block;
    block.reserve(extracted.size() + 1);
    for (size_t k : ntIdx) block.push_back(std::move(extracted[k]));
    const int opPos = index + static_cast<int>(ntIdx.size());
    block.push_back(std::move(op));
    for (size_t k : thIdx) block.push_back(std::move(extracted[k]));
    m_operations.insert(m_operations.begin() + index,
                        std::make_move_iterator(block.begin()),
                        std::make_move_iterator(block.end()));
    if (m_breakpoint >= index) m_breakpoint++;
    m_currentIndex = limit + 1;

    // 4. The threads re-cut on the new geometry.
    const int thBase = opPos + 1;
    bool replayFailed = false;
    for (int t = 0; t < static_cast<int>(thIdx.size()); ++t) {
        Operation* s = m_operations[thBase + t].get();
        if (!s->execute(doc)) {
            // E.g. the thread's cylindrical span was consumed. The user's op
            // DID apply; the step suspends with the explainer banner.
            std::fprintf(stderr, "[History] reflow: step %d '%s' could not "
                                 "re-execute\n",
                         thBase + t, s->name().c_str());
            m_failedReplayAt = thBase + t;
            replayFailed = true;
            break;
        }
        s->rememberGoodParams();
    }

    // Finishing passes propagate to bodies the inserted op CREATED - a split
    // lengthwise through a bolt must leave threads on BOTH halves, not just
    // the half that kept the original body id ("half of it is smooth").
    // Clone each displaced thread for each created body and append the
    // clones as real (undoable, saveable) steps. Gate on THIS replay's
    // outcome - a stale m_failedReplayAt from an earlier suspension must
    // not silently skip propagation.
    if (!replayFailed) {
        OperationDiff nd = m_operations[opPos]->captureDiff();
        for (int createdId : nd.created) {
            for (int t = 0; t < static_cast<int>(thIdx.size()); ++t) {
                Operation* s = m_operations[thBase + t].get();
                std::unique_ptr<Operation> clone = s->cloneForBody(createdId);
                if (!clone) continue;
                if (!clone->execute(doc)) {
                    std::fprintf(stderr, "[History] reflow: thread clone for "
                                         "body %d failed (span may not "
                                         "exist on this piece)\n", createdId);
                    continue;
                }
                clone->rememberGoodParams();
                m_operations.insert(m_operations.begin() + m_currentIndex + 1,
                                    std::move(clone));
                m_currentIndex++;
            }
        }
    }
    if (m_eventBus)
        m_eventBus->publish(materializr::HistoryStepEvent{m_currentIndex, false});
    return true;
}
